#include "ui_controller/application_controller.h"

#include <chrono>
#include <exception>
#include <future>
#include <stdexcept>
#include <utility>

#include "appcore/activity.h"
#include "appcore/cancel_router.h"
#include "appcore/readiness.h"
#include "appcore/shutdown_plan.h"
#include "audio/player.h"
#include "base/log.h"
#include "dictation/hotkey_runtime.h"
#include "dictation/transcription_runtime.h"
#include "focus/capture_service.h"
#include "recorder/recorder.h"
#include "remote/remote_backend.h"
#include "settings/store.h"
#include "speech/dictation_language.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"services.application_controller"};

// Quit cancels the queued speech jobs and waits this long for the window being decoded, which
// cannot be interrupted (the C ABI has no abort; architecture 5.5).
constexpr std::chrono::milliseconds kSpeechShutdownBudget{5000};

// RemoteSpeechBackend's readout before any connection attempt failed: its last_error is empty.
constexpr std::string_view kRemoteNotConnected = "Remote engine is not connected";

template <class T> T& required(T* pointer, const char* name) {
    if (pointer == nullptr) {
        throw std::invalid_argument(std::string("ApplicationController needs ") + name);
    }
    return *pointer;
}

} // namespace

ApplicationController::ApplicationController(ApplicationServices services)
    : services_(std::move(services)), settings_(required(services_.settings, "settings")),
      activity_(required(services_.activity, "the activity state")),
      speech_(required(services_.speech, "the speech service")),
      transcription_(required(services_.transcription, "the transcription runtime")),
      cancel_(required(services_.cancel, "the cancel router")),
      ui_(required(services_.ui, "the UI executor")), reload_(ui_) {
    required(services_.startup, "the startup executor");
    required(services_.jobs, "the jobs executor");
    if (!services_.resolve_engine) {
        throw std::invalid_argument("ApplicationController needs an engine resolver");
    }
    // The user's Cancel (application_controller.py:1129-1142): the recording or transcription in
    // progress, else an AI walkthrough write-up still running. The actions still running join
    // the router with their runtime (ActionWiring).
    cancel_.add(0, [this] { return transcription_.cancel(); });
    cancel_.add(10, [this] { return transcription_.cancel_write_up(); });
    links_.push_back(cancel_.nothing_to_cancel.connect(
        ui_, [this](std::string text) { nothing_to_cancel.emit(text); }));

    reload_.on_activity(activity_.snapshot());
    links_.push_back(activity_.changed.connect(
        ui_, [this](ActivitySnapshot snapshot) { reload_.on_activity(snapshot); }));
    drive_engine();
}

ApplicationController::~ApplicationController() {
    alive_.reset();
    links_.clear();
    reload_.shutdown();
}

Json ApplicationController::settings_snapshot() const {
    return *settings_.snapshot();
}

// ---- RecordingControl ----------------------------------------------------------------------

bool ApplicationController::is_recording() const {
    return transcription_.is_recording();
}

bool ApplicationController::recorder_busy() const {
    return transcription_.recorder_busy();
}

bool ApplicationController::canceling_recording() const {
    return transcription_.canceling_recording();
}

bool ApplicationController::capture_canceled() const {
    return transcription_.capture_canceled();
}

bool ApplicationController::stream_open() const {
    return transcription_.stream_open();
}

Signal<bool>& ApplicationController::recording_state_changed() {
    return transcription_.recording_state_changed();
}

// application_controller.py:1129-1142, with _cancel_actions' NOTHING_TO_CANCEL from the router.
bool ApplicationController::cancel() {
    return cancel_.cancel();
}

// application_controller.py:1150-1157: the window changes on the UI thread.
void ApplicationController::minimize_to_tray() {
    minimize_to_tray_requested.emit();
}

void ApplicationController::paste_last_original() {
    paste_last_original_requested.emit();
}

// ---- recording -----------------------------------------------------------------------------

// _refuse_dictation_during_meeting (1096-1109).
bool ApplicationController::refuse_dictation() {
    const ActivitySnapshot activity = activity_.snapshot();
    const std::optional<std::string> refusal = ::fv::refuse_dictation(activity);
    if (!refusal) {
        return false;
    }
    if (activity.meeting_active && !activity.backup_in_progress && !activity.meeting_setup_open) {
        kLog.info("Dictation start refused: Meeting Mode is active");
    }
    status_update.emit(*refusal);
    return true;
}

// start_recording (996-1014).
bool ApplicationController::start_recording(std::string source) {
    if (refuse_dictation()) {
        return false;
    }
    if (const std::optional<std::string> unavailable = transcription_readiness_message()) {
        status_update.emit(*unavailable);
        return false;
    }
    dictation::StartOptions options;
    options.source = std::move(source);
    return transcription_.start_recording(std::move(options));
}

void ApplicationController::stop_recording() {
    transcription_.stop_recording();
}

bool ApplicationController::toggle_action_recording(const std::string& action_id) {
    if (transcription_.is_recording()) {
        transcription_.stop_recording();
        return true;
    }
    if (refuse_dictation())
        return false;
    if (const auto unavailable = transcription_readiness_message()) {
        status_update.emit(*unavailable);
        return false;
    }
    dictation::StartOptions options;
    options.mode = dictation::JobMode::Action;
    options.source = "hotkey";
    options.action_id = action_id;
    return transcription_.start_recording(std::move(options));
}

bool ApplicationController::toggle_route_recording() {
    if (transcription_.is_recording()) {
        transcription_.stop_recording();
        return true;
    }
    if (refuse_dictation())
        return false;
    if (const auto unavailable = transcription_readiness_message()) {
        status_update.emit(*unavailable);
        return false;
    }
    dictation::StartOptions options;
    options.source = "hotkey";
    options.route = "hotkey";
    return transcription_.start_recording(std::move(options));
}

// toggle_recording (1065-1080): the gates only when not recording, so a stop always works.
bool ApplicationController::toggle_recording() {
    const bool recording = transcription_.is_recording();
    if (!recording && refuse_dictation()) {
        return false;
    }
    if (!recording) {
        if (const std::optional<std::string> unavailable = transcription_readiness_message()) {
            status_update.emit(*unavailable);
            return false;
        }
    }
    transcription_.toggle_recording();
    return true;
}

// start_profile_recording (1016-1019).
bool ApplicationController::start_profile_recording(const std::string& profile_id,
                                                    std::string source) {
    if (refuse_dictation()) {
        return false;
    }
    dictation::StartOptions options;
    options.source = std::move(source);
    options.profile_id = profile_id;
    return transcription_.start_recording(std::move(options));
}

// toggle_profile_recording (1021-1027): a shortcut pressed during capture only stops that
// capture.
void ApplicationController::toggle_profile_recording(const std::string& profile_id) {
    if (transcription_.is_recording()) {
        stop_recording();
    } else {
        start_profile_recording(profile_id);
    }
}

// retranscribe_audio (1197-1199).
void ApplicationController::retranscribe_audio(const std::filesystem::path& audio_path) {
    if (!refuse_dictation()) {
        transcription_.retranscribe_audio(audio_path);
    }
}

// on_model_changed (1213-1219). The runtime prompts the Downloads flow forgets arrive with it.
void ApplicationController::on_model_changed(const std::string& backend_id) {
    if (transcription_.is_recording() || is_transcribing()) {
        status_update.emit("Finish recording or transcription before changing engines");
        return;
    }
    if (!refuse_dictation()) {
        if (services_.forget_runtime_prompts) {
            services_.forget_runtime_prompts();
        }
        transcription_.on_model_changed(backend_id);
    }
}

// change_audio_device (969-985). The saved order is applied to the same recorder, so the level,
// error and switch connections made at startup keep reaching the UI (Python rewired them on the
// recorder it rebuilt). Reading the microphone list blocks, so it runs on io.
void ApplicationController::change_audio_device() {
    if (transcription_.is_recording() || transcription_.has_active_job()) {
        kLog.warning("Cannot change audio device while recording");
        status_update.emit("Wait for the recording to finish before changing device");
        return;
    }
    // The recorder's next recording opens another microphone: windows decoded early from the
    // last one's tape are not this file's.
    transcription_.discard_early_decode();
    Recorder* recorder = services_.recorder;
    auto load = services_.load_priority;
    const auto applied = [this, alive = std::weak_ptr<int>(alive_)] {
        ui_.post([this, alive] {
            if (alive.lock()) {
                kLog.info("Microphone order changed; recorder rebuilt");
                status_update.emit("Audio device changed");
            }
        });
    };
    if (recorder == nullptr || !load || services_.io == nullptr) {
        applied();
        return;
    }
    services_.io->post([recorder, load, applied] {
        try {
            recorder->set_device_priority(load());
        } catch (const std::exception& error) {
            kLog.warning("Couldn't read the saved microphone order ({})", error.what());
        }
        applied();
    });
}

// _on_local_engine_settings_changed (953-958).
void ApplicationController::on_local_engine_settings_changed() {
    if (speech::engine(settings_snapshot()) == "remote") {
        // They apply to this computer's engines, which Remote isn't using: reloading would
        // only drop and reopen the connection to the paired computer.
        return;
    }
    reload_whisper_model();
}

// on_settings_changed (440-452). Transforms, actions and the newer standard shortcuts register
// as global shortcuts. Automation (M5) re-reads its settings here later. "updates" needs nothing
// here: the updater watches its three keys and schedules its check itself
// (update::UpdaterRuntime).
void ApplicationController::on_settings_changed(std::string_view kind) {
    kLog.debug("Settings of kind {} changed", kind);
    if ((kind == "transforms" || kind == "hotkeys" || kind == "actions") && hotkeys_ != nullptr) {
        hotkeys_->refresh_profile_hotkeys();
    }
}

void ApplicationController::update_hotkeys(const hotkeys::HotkeyList& hotkeys) {
    if (hotkeys_ != nullptr) {
        hotkeys_->update_hotkeys(hotkeys);
    }
}

void ApplicationController::update_recording_trigger_mode(std::string_view mode) {
    if (hotkeys_ != nullptr) {
        hotkeys_->set_recording_trigger_mode(mode);
    }
}

// ---- engine --------------------------------------------------------------------------------

// reload_whisper_model (454-475): the machine refuses during a meeting or a recording, with
// the same statuses.
// Safe from any thread: other threads see the reload coming at once, and the engine choice is
// read on the UI thread before the debounce ends.
void ApplicationController::reload_whisper_model(std::string_view note) {
    if (ui_.is_current()) {
        reload_.select(speech::engine(settings_snapshot()), {});
        reload_.request(note);
        return;
    }
    reload_.request(note);
    ui_.post([this, alive = std::weak_ptr<int>(alive_)] {
        if (alive.lock()) {
            reload_.select(speech::engine(settings_snapshot()), {});
        }
    });
}

// transcription_readiness_message (708-732) with _speech_engine_readiness_message (1481-1491).
std::optional<std::string> ApplicationController::transcription_readiness_message() {
    const std::string engine = speech::engine(settings_snapshot());
    const speech::Readiness readiness = speech_.readiness(speech::Slot::Dictation);
    appcore::ReadinessInput input;
    input.has_backend = !engine.empty();
    input.backend_id = engine;
    input.available = readiness.state == speech::ReadyState::Ready;
    input.reload_in_flight = reload_.in_flight();
    input.model_missing = readiness.state == speech::ReadyState::ModelMissing;
    input.has_last_error = readiness.state != speech::ReadyState::Ready &&
                           readiness.state != speech::ReadyState::Loading &&
                           !readiness.device_info.empty();
    input.device_info = readiness.device_info;
    if (engine == "remote") {
        // _remote_engine_readiness_message reads the backend's last_error, empty until a
        // connection attempt failed.
        input.has_last_error = input.has_last_error && readiness.device_info != kRemoteNotConnected;
        input.remote_restoring = remote_.restoring && remote_.restoring();
        input.remote_host = remote_.host_name ? remote_.host_name() : std::string();
    }
    const appcore::ReadinessDecision decision = appcore::transcription_readiness_message(input);
    if (decision.request_remote_retry && remote_.retry_now) {
        // A quiet reconnect on the link's own executor, rather than a full engine reload.
        remote_.retry_now();
    }
    if (decision.request_reload) {
        reload_whisper_model();
    }
    if (decision.request_model_consent) {
        model_setup_requested.emit();
    }
    return decision.message;
}

// is_transcribing (734-739).
bool ApplicationController::is_transcribing() const {
    return transcription_.has_active_job() || transcription_.is_transcribing();
}

bool ApplicationController::is_meeting_active() const {
    return activity_.snapshot().meeting_active;
}

// release_local_engine (651-668): the speech service releases the slot. A load still running
// cannot be interrupted, so the engine it brings up is released when it ends; a warm-up is not a
// load, so the engine it warms is released now (the release is queued behind the warm-up on the
// speech thread, as OpenWhisper's cleanup ends the worker its warm-up runs on).
bool ApplicationController::release_local_engine() {
    if (speech::engine(settings_snapshot()) == "remote") {
        // A remote engine's weights are on the host: closing its connection would free nothing
        // here, so the meeting leaves it, and its live preview, alone.
        return false;
    }
    const bool loading = loading_.has_value();
    const bool released = reload_.release_for_lease();
    if (!released) {
        return false;
    }
    if (loading) {
        release_after_load_ = true;
    } else {
        speech_.release_local(speech::Slot::Dictation);
    }
    return true;
}

// restore_local_engine (670-694): a no-op unless a release happened; the reload takes the same
// path as any other.
void ApplicationController::restore_local_engine() {
    reload_.restore_after_lease();
}

// ---- the Remote engine ------------------------------------------------------------------

std::optional<remote::RemoteModels> ApplicationController::remote_models() const {
    return remote_.model_choices ? remote_.model_choices() : std::nullopt;
}

std::optional<remote::RemoteLink> ApplicationController::remote_link() const {
    return remote_.link ? remote_.link() : std::nullopt;
}

void ApplicationController::select_remote_model(const std::string& family,
                                                const std::string& model) {
    const auto state = remote_models();
    if (!state || speech::engine(settings_snapshot()) != "remote" || !remote_.request_model) {
        return;
    }
    const auto plan =
        remote::plan_remote_model_switch(*state, family, model, is_meeting_active(),
                                         transcription_.is_recording() || is_transcribing());
    if (plan.skip) {
        return;
    }
    if (!plan.refusal.empty()) {
        status_update.emit(plan.refusal);
        remote_models_stale.emit();
        return;
    }
    remote_.request_model(family, model, std::nullopt);
    reload_whisper_model(plan.note);
}

void ApplicationController::select_remote_runtime(const std::string& family,
                                                  const std::string& model, const Json& changes) {
    const auto state = remote_models();
    if (!state || speech::engine(settings_snapshot()) != "remote" || !remote_.request_model) {
        return;
    }
    const bool busy = is_meeting_active() || transcription_.is_recording() || is_transcribing() ||
                      !reload_.settled();
    const auto plan = remote::plan_remote_runtime_change(*state, family, model, busy);
    if (plan.skip) {
        return;
    }
    if (!plan.refusal.empty()) {
        status_update.emit(plan.refusal);
        remote_models_stale.emit();
        return;
    }
    remote_.request_model(family, model, std::optional<Json>(std::in_place, changes));
    reload_whisper_model(plan.note);
}

void ApplicationController::retry_remote() {
    if (remote_.retry_now) {
        remote_.retry_now();
    }
}

void ApplicationController::remote_link_poked() {
    ui_.post([this, alive = std::weak_ptr<int>(alive_)] {
        if (alive.lock()) {
            remote_link_changed.emit();
        }
    });
}

void ApplicationController::remote_clients_poked() {
    if (!remote_.connected_clients) {
        return;
    }
    auto clients = remote_.connected_clients();
    ui_.post([this, alive = std::weak_ptr<int>(alive_), clients = std::move(clients)] {
        if (alive.lock()) {
            remote_clients_changed.emit(clients);
        }
    });
}

std::function<bool()> ApplicationController::lease_release_hook(std::chrono::milliseconds wait) {
    return [this, alive = std::weak_ptr<int>(alive_), wait] {
        if (!alive.lock()) {
            return false;
        }
        if (ui_.is_current()) {
            return release_local_engine();
        }
        auto answer = std::make_shared<std::promise<bool>>();
        auto released = answer->get_future();
        ui_.post([this, alive, answer] {
            answer->set_value(alive.lock() ? release_local_engine() : false);
        });
        if (released.wait_for(wait) != std::future_status::ready) {
            // The release still runs when the UI thread gets to it, and owes its restore then.
            kLog.warning("The UI thread did not release the speech engine in time for the meeting");
            return false;
        }
        return released.get();
    };
}

std::function<void()> ApplicationController::lease_restore_hook() {
    return [this, alive = std::weak_ptr<int>(alive_)] {
        if (!alive.lock()) {
            return;
        }
        if (ui_.is_current()) {
            restore_local_engine();
            return;
        }
        ui_.post([this, alive] {
            if (alive.lock()) {
                restore_local_engine();
            }
        });
    };
}

// The reload driver: the machine decides, the speech service does.
void ApplicationController::drive_engine() {
    links_.push_back(reload_.start_load.connect(ui_, [this] { start_engine_load(); }));
    links_.push_back(reload_.start_warmup.connect(ui_, [this] {
        warming_ = true;
        speech_.warm_up(speech::Slot::Dictation);
    }));
    links_.push_back(reload_.discard_load.connect(
        ui_, [this] { speech_.release_local(speech::Slot::Dictation); }));
    links_.push_back(
        reload_.ensure_model_available.connect(ui_, [this] { model_setup_requested.emit(); }));
    links_.push_back(
        reload_.busy_changed.connect(ui_, [this](bool busy) { engine_busy_changed.emit(busy); }));
    links_.push_back(
        reload_.status.connect(ui_, [this](std::string text) { status_update.emit(text); }));
    links_.push_back(
        reload_.device_info.connect(ui_, [this](std::string info, std::optional<bool> usable) {
            device_info_update.emit(info, usable);
        }));
    links_.push_back(reload_.missing_runtime.connect(
        ui_, [this](std::string model) { runtime_consent_requested.emit(model); }));
    // streaming_setup_due is StreamingRuntime::attach's: it sets the preview up after a load.
    links_.push_back(speech_.readiness_changed.connect(
        ui_, [this](speech::Slot slot, speech::Readiness readiness) {
            if (slot == speech::Slot::Dictation) {
                track_loaded(readiness);
            }
            on_engine_readiness(slot, readiness);
        }));
    links_.push_back(speech_.warmed_up.connect(
        ui_, [this](speech::Slot slot, bool) { on_engine_warmed(slot); }));
}

// _reload_selected_engine's load half: the selected engine's config, resolved now from the
// saved settings, goes to the speech service; its readiness comes back as the load's answer.
void ApplicationController::start_engine_load() {
    const Json settings = settings_snapshot();
    speech::BackendConfig config;
    try {
        config = services_.resolve_engine(settings);
    } catch (const std::exception& error) {
        kLog.error_with(error, "Could not resolve the speech engine");
        reload_.on_load_finished(
            speech::Readiness{speech::ReadyState::Failed, error.what(), error.what()});
        return;
    }
    config.settings = std::make_shared<const Json>(settings);
    reload_.select(config.backend, config.model_key);
    loading_ = config;
    selected_ = config;
    load_announced_ = false;
    speech_.select(speech::Slot::Dictation, std::move(config));
}

void ApplicationController::select_engine_now() {
    const Json settings = settings_snapshot();
    speech::BackendConfig config;
    try {
        config = services_.resolve_engine(settings);
    } catch (const std::exception& error) {
        kLog.error_with(error, "Could not resolve the speech engine");
        return;
    }
    config.settings = std::make_shared<const Json>(settings);
    reload_.select(config.backend, config.model_key);
    selected_ = config;
    speech_.select(speech::Slot::Dictation, std::move(config));
}

// What the slot holds: the selected config from its Ready on, nothing once it is released,
// failed or missing a part. A load or warm-up keeps what was there until it answers.
void ApplicationController::track_loaded(const speech::Readiness& readiness) {
    switch (readiness.state) {
    case speech::ReadyState::Loading:
    case speech::ReadyState::Warming:
        return;
    case speech::ReadyState::Ready: {
        const std::lock_guard lock(loaded_mutex_);
        loaded_ = selected_;
        if (selected_ && loaded_runtime_dir_.empty() && selected_->backend == "parakeet") {
            loaded_runtime_dir_ = selected_->runtime_dir;
        }
        return;
    }
    default: {
        const std::lock_guard lock(loaded_mutex_);
        loaded_.reset();
        return;
    }
    }
}

std::optional<speech::BackendConfig> ApplicationController::loaded_engine() const {
    const std::lock_guard lock(loaded_mutex_);
    return loaded_;
}

std::filesystem::path ApplicationController::loaded_runtime_dir() const {
    const std::lock_guard lock(loaded_mutex_);
    return loaded_runtime_dir_;
}

void ApplicationController::refresh_api_engine() {
    if (speech::engine(settings_snapshot()) != "api" || !reload_.settled() ||
        transcription_.is_recording() || is_transcribing()) {
        return;
    }
    select_engine_now();
}

void ApplicationController::on_engine_readiness(speech::Slot slot,
                                                const speech::Readiness& readiness) {
    if (slot != speech::Slot::Dictation) {
        return;
    }
    if (!loading_) {
        on_remote_link(readiness);
        return;
    }
    // A select announces Loading at once, in order behind whatever the slot reported before
    // (a release's Offline among them); the load's answer is the first readiness after it.
    if (readiness.state == speech::ReadyState::Loading) {
        load_announced_ = true;
        return;
    }
    if (!load_announced_) {
        return;
    }
    const speech::BackendConfig config = std::move(*loading_);
    loading_.reset();
    load_announced_ = false;
    if (readiness.state == speech::ReadyState::Ready && services_.runtime_loaded) {
        try {
            services_.runtime_loaded(config);
        } catch (const std::exception& error) {
            kLog.error_with(error, "Could not lease the loaded speech runtime");
        }
    }
    if (std::exchange(release_after_load_, false) && readiness.state == speech::ReadyState::Ready) {
        // The lease that waited for this load frees what it loaded, before the machine hears of
        // it: its finish may start the restore reload at once, which this must not release.
        speech_.release_local(speech::Slot::Dictation);
    }
    remote_reported_ = readiness.device_info;
    reload_.on_load_finished(readiness);
}

void ApplicationController::on_remote_link(const speech::Readiness& readiness) {
    if (!selected_ || selected_->backend != "remote" || !reload_.settled() ||
        reload_.released_for_lease()) {
        return; // a reload reports its own result
    }
    const bool available = readiness.state == speech::ReadyState::Ready;
    if (!available && readiness.state != speech::ReadyState::Offline &&
        readiness.state != speech::ReadyState::Failed) {
        return;
    }
    if (available || readiness.device_info != remote_reported_) {
        device_info_update.emit(readiness.device_info, available);
    }
    remote_reported_ = readiness.device_info;
    if (available && services_.setup_streaming) {
        // The preview may be waiting for a first connection.
        services_.setup_streaming();
    }
}

void ApplicationController::on_engine_warmed(speech::Slot slot) {
    if (slot != speech::Slot::Dictation || !warming_) {
        return;
    }
    warming_ = false;
    reload_.on_warmup_finished();
}

// ---- startup -------------------------------------------------------------------------------

// notify_main_ui_ready (830-894): the first load of a speech engine, Parakeet's model or the
// Remote engine's connection (the preview is set up once it has settled), or for another engine
// no load and the preview's set-up now; then the deferred database step. The API engine has no
// model to load: its slot gets it at once, outside the reload machine, so that its readiness
// says whether a key resolves.
void ApplicationController::notify_main_ui_ready() {
    const std::string engine = speech::engine(settings_snapshot());
    if (engine == "parakeet" || engine == "remote") {
        reload_.select(engine, {});
        reload_.initial_load();
    } else {
        if (engine == "api") {
            select_engine_now();
        }
        if (services_.setup_streaming) {
            ui_.post([setup = services_.setup_streaming, alive = std::weak_ptr<int>(alive_)] {
                if (alive.lock()) {
                    setup();
                }
            });
        }
    }
    if (!startup_database_requested_) {
        startup_database_requested_ = true;
        services_.startup->post([this, alive = std::weak_ptr<int>(alive_)] {
            if (alive.lock()) {
                initialize_startup_database();
            }
        });
    }
    if (services_.start_background_jobs) {
        services_.start_background_jobs();
    }
}

// _initialize_startup_database (896-904).
void ApplicationController::initialize_startup_database() {
    std::string error;
    try {
        if (services_.ensure_database) {
            services_.ensure_database();
        }
    } catch (const std::exception& failure) {
        error = failure.what();
        if (error.empty()) {
            error = "Could not initialize the database";
        }
        kLog.error_with(failure, "Could not initialize the database");
    }
    if (shutting_down_) {
        return;
    }
    ui_.post([this, error, alive = std::weak_ptr<int>(alive_)] {
        if (alive.lock()) {
            on_startup_database_ready(error);
        }
    });
}

// _on_startup_database_ready (906-919), M1's dependents: recovery of interrupted recordings.
void ApplicationController::on_startup_database_ready(const std::string& error) {
    if (shutting_down_) {
        return;
    }
    if (!error.empty()) {
        update_readiness_error_ = error;
        status_update.emit(error);
        return;
    }
    startup_database_ready_ = true;
    activity_.set_startup_ready(true);
    activity_.set_recovery_running(true);
    auto recover = services_.recover_recordings;
    if (!recover) {
        recover = [runtime = &transcription_] {
            runtime->recover_recordings();
        };
    }
    ActivityState* activity = &activity_;
    services_.jobs->post([recover, activity] {
        try {
            recover();
        } catch (const std::exception& failure) {
            kLog.error_with(failure, "Recording recovery failed");
        }
        activity->set_recovery_running(false);
    });
    if (services_.start_database_dependents) {
        services_.start_database_dependents();
    }
}

// ---- quit ----------------------------------------------------------------------------------

// cleanup (1298-1357), the steps M1 has.
void ApplicationController::add_shutdown_steps(ShutdownPlan& plan) {
    plan.add(ShutdownStep::StartupWork, {}, [this] {
        shutting_down_ = true;
        activity_.set_shutting_down(true);
    });
    plan.add(ShutdownStep::TranscriptionBegin, {}, [this] { transcription_.begin_shutdown(); });
    plan.add(ShutdownStep::PlaybackAndFocus, "Error stopping playback or focus capture", [] {
        audio::stop_playback();
        // Its capture threads hold accessibility (COM) objects that must be released first.
        focus::shutdown_service();
    });
    plan.add(ShutdownStep::CancelTranscription, "Error canceling transcription", [this] {
        if (transcription_.is_transcribing()) {
            kLog.info("Canceling ongoing transcription...");
        }
    });
    plan.add(ShutdownStep::Timers, "Error stopping watchdog timers",
             [this] { reload_.shutdown(); });
    plan.add(ShutdownStep::InterruptBackends, "Error interrupting the speech engine",
             [this] { speech_.cancel_all(kSpeechShutdownBudget); });
    plan.add(ShutdownStep::HotkeyRuntime, "Error during hotkey runtime cleanup", [this] {
        if (hotkeys_ != nullptr) {
            hotkeys_->cleanup();
        }
    });
    plan.add(ShutdownStep::Recorder, "Error during recorder cleanup", [this] {
        if (services_.recorder != nullptr) {
            services_.recorder->cleanup();
        }
    });
}

} // namespace fv
