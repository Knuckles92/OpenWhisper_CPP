#include "dictation/transcription_runtime.h"

#include <algorithm>
#include <exception>
#include <format>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "appcore/activity.h"
#include "appcore/preview_throttle.h"
#include "audio/player.h"
#include "base/events.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/metrics.h"
#include "base/utf8.h"
#include "dictation/cleanup_stage.h"
#include "dictation/command_selection.h"
#include "dictation/events.h"
#include "dictation/messages.h"
#include "dictation/pipeline.h"
#include "dictation/recognition.h"
#include "dictation/recovery.h"
#include "dictation/streaming_runtime.h"
#include "dictation/text_preparer.h"
#include "engine/error.h"
#include "history/dictation_stats.h"
#include "history/entry.h"
#include "llm/transcript_cleanup.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "settings/model_selection.h"
#include "settings/store.h"
#include "speech/dictation_language.h"
#include "speech/file_windows.h"
#include "speech/incremental_session.h"
#include "speech/service.h"
#include "text/cleanup_profiles.h"
#include "text/transforms.h"
#include "walkthrough/finish.h"
#include "walkthrough/renderer.h"

namespace fv::dictation {

namespace {

constexpr log::Logger kLog{"services.runtime.transcription"};

// Shown when the full pass found nothing but the live preview did. Names the source, because the
// text on screen is the preview's and is not saved.
constexpr std::string_view EMPTY_PREVIEW_FALLBACK_MESSAGE =
    "No speech detected — showing the live preview (not saved)";
// _raise_if_canceled's message, the one an engine cancelled mid-decode ends with too.
constexpr std::string_view TRANSCRIPTION_CANCELED = "Transcription canceled";
// A file's decode found another engine loaded than the one it was sent to (an engine switch
// landed between the two).
constexpr std::string_view kEngineSwitchedMessage =
    "The speech engine changed while this audio was being transcribed. Try again.";

template <class T> T& required(T* pointer, const char* name) {
    if (pointer == nullptr) {
        throw std::invalid_argument(std::format("TranscriptionRuntime needs {}", name));
    }
    return *pointer;
}

bool blank(std::string_view text) {
    return py::strip(text).empty();
}

// Python's OSError.strerror: the system's text for `code`, without the trailing full stop and
// line break FormatMessage adds on Windows.
std::string strerror_text(const std::error_code& code) {
    std::string text = code.message();
    while (!text.empty() &&
           (static_cast<unsigned char>(text.back()) <= ' ' || text.back() == '.')) {
        text.pop_back();
    }
    return text;
}

ContextFields to_context(const Json& fields) {
    ContextFields context;
    if (!fields.is_object()) {
        return context;
    }
    for (const auto& [key, value] : fields.items()) {
        if (value.is_string()) {
            context[key] = value.get<std::string>();
        }
    }
    return context;
}

std::filesystem::path absolute_of(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal();
}

// _dictation_route: "auto" when spoken routing also listens to plain dictations; never throws.
std::string dictation_route(const Json& settings) {
    try {
        if (keys::voice_routing_enabled.read(settings) &&
            keys::voice_routing_on_dictation.read(settings)) {
            return "auto";
        }
    } catch (...) {
        kLog.warning("Routing setting unavailable; spoken routing off for this dictation");
    }
    return {};
}

// The kind of failure a filter or the judge raised, as Python's type(exc).__name__ names it: the
// message may quote the dictated words, so it is never logged.
std::string_view error_kind(const std::exception& error) {
    if (const auto* python = dynamic_cast<const py::PyError*>(&error)) {
        return python->type_name();
    }
    // A transform's model refusing (text_rewrite's refusals) is its own Python class.
    if (dynamic_cast<const CommandRefused*>(&error) != nullptr) {
        return "CommandRefused";
    }
    if (dynamic_cast<const std::system_error*>(&error) != nullptr) {
        return "OSError";
    }
    if (dynamic_cast<const std::runtime_error*>(&error) != nullptr) {
        return "RuntimeError";
    }
    return "Exception";
}

// The saved id of an action found by `requested`: the port fills it (find_action strips what it
// is asked for), and an action found without one is the one asked for.
std::string action_key(const RecordingAction& action, std::string_view requested) {
    return action.id.empty() ? std::string(requested) : action.id;
}

bool is_local_engine(std::string_view engine) {
    // Python's LocalSpeechBackend family: the in-process engine and the paired host's.
    return engine == "parakeet" || engine == "remote";
}

// Clears the decoding flag however the decode ends.
struct DecodingScope {
    explicit DecodingScope(std::atomic<bool>& flag) : flag_(flag) { flag_ = true; }
    ~DecodingScope() { flag_ = false; }
    DecodingScope(const DecodingScope&) = delete;
    DecodingScope& operator=(const DecodingScope&) = delete;
    std::atomic<bool>& flag_;
};

} // namespace

// A finished transcript: (text, raw_text, info) as _maybe_cleanup_transcript returns it.
struct TranscriptionRuntime::Completion {
    std::string text;
    std::optional<std::string> raw_text;
    std::optional<CleanupInfo> info;
};

// One History save: the entry, the job its event names, and the failed entry a retry completes.
struct TranscriptionRuntime::QueuedEntry {
    AddEntry fields;
    std::string job_id;
    std::optional<std::string> retry_entry_id;
};

TranscriptionRuntime::TranscriptionRuntime(TranscriptionServices services)
    : services_(std::move(services)), settings_(required(services_.settings, "settings")),
      recorder_(required(services_.recorder, "a recorder")),
      speech_(required(services_.speech, "the speech service")),
      history_(required(services_.history, "History")),
      stats_(required(services_.stats, "the stats repository")),
      activity_(required(services_.activity, "the activity state")),
      ui_(required(services_.ui, "the UI executor")),
      jobs_(required(services_.jobs, "the jobs executor")),
      persistence_(required(services_.persistence, "the persistence executor")),
      events_(services_.events != nullptr ? *services_.events : null_event_sink()),
      clock_(services_.clock != nullptr ? *services_.clock : system_clock()),
      incremental_(std::make_unique<speech::IncrementalDictation>(speech_)) {
    required(services_.delivery.clipboard, "a clipboard");
    required(services_.delivery.keys, "synthetic keys");
    model_consent_ = services_.hooks.request_model_consent;
    capture_link_ = recorder_.capture_error().connect(
        ui_, [this](std::string message) { on_capture_error(std::move(message)); });
}

TranscriptionRuntime::~TranscriptionRuntime() {
    capture_link_.disconnect();
    preview_link_.disconnect();
    preview_throttle_.reset();
    alive_.reset();
}

// --- RecordingControl --------------------------------------------------------------------

bool TranscriptionRuntime::is_recording() const {
    return recorder_.is_recording();
}

bool TranscriptionRuntime::recorder_busy() const {
    return recorder_.recorder_busy();
}

bool TranscriptionRuntime::canceling_recording() const {
    return canceling_recording_.load();
}

bool TranscriptionRuntime::capture_canceled() const {
    return recorder_.capture_canceled();
}

bool TranscriptionRuntime::stream_open() const {
    return recorder_.stream_open();
}

bool TranscriptionRuntime::start(std::string source) {
    StartOptions options;
    options.source = std::move(source);
    return start_recording(std::move(options));
}

void TranscriptionRuntime::stop() {
    stop_recording();
}

bool TranscriptionRuntime::toggle() {
    return toggle_recording();
}

void TranscriptionRuntime::minimize_to_tray() {
    if (services_.hooks.minimize_to_tray) {
        services_.hooks.minimize_to_tray();
    }
}

void TranscriptionRuntime::paste_last_original() {
    if (services_.hooks.paste_last_original) {
        services_.hooks.paste_last_original();
    }
}

// --- state ------------------------------------------------------------------------------

bool TranscriptionRuntime::has_active_job() const {
    return slot_.has_active_job();
}

bool TranscriptionRuntime::cancelable() const {
    return slot_.working() || (recorder_.is_recording() && !recorder_.capture_canceled());
}

bool TranscriptionRuntime::is_transcribing() const {
    return decoding_.load();
}

std::shared_ptr<const DictationJob> TranscriptionRuntime::current_job() const {
    return slot_.current_job();
}

PendingAudio TranscriptionRuntime::pending() const {
    std::lock_guard lock(state_mutex_);
    return pending_;
}

void TranscriptionRuntime::set_pending(PendingAudio pending) {
    std::lock_guard lock(state_mutex_);
    pending_ = std::move(pending);
}

std::string TranscriptionRuntime::pending_streaming_text() const {
    std::lock_guard lock(state_mutex_);
    return pending_streaming_text_;
}

Json TranscriptionRuntime::settings_snapshot() const {
    return *settings_.snapshot();
}

double TranscriptionRuntime::seconds_since(std::chrono::steady_clock::time_point start) const {
    return std::chrono::duration<double>(clock_.steady() - start).count();
}

void TranscriptionRuntime::post_ui(std::function<void()> task) {
    std::weak_ptr<int> alive = alive_;
    ui_.post([alive, task = std::move(task)] {
        if (alive.lock()) {
            task();
        }
    });
}

void TranscriptionRuntime::post_jobs(std::function<void()> task) {
    std::weak_ptr<int> alive = alive_;
    jobs_.post([alive, task = std::move(task)] {
        if (alive.lock()) {
            task();
        }
    });
}

void TranscriptionRuntime::notify(const std::string& text, bool done) {
    if (!text.empty()) {
        notice_requested.emit(text, done);
    }
}

void TranscriptionRuntime::announce_recording(bool recording) {
    activity_.set_recording(recording);
    recording_state_changed_.emit(recording);
}

void TranscriptionRuntime::publish_preview(const DictationJob* job, std::string_view text,
                                           bool final) {
    slot_.with_preview(job, [&] {
        events_.publish(events::TRANSCRIPTION_PREVIEW,
                        events::transcription_preview(job, text, final), false);
    });
}

void TranscriptionRuntime::publish_preview(std::string_view job_id, std::string_view text,
                                           bool final) {
    // with_preview checks the job again under the slot's lock.
    const auto job = slot_.previewing();
    if (job && job->id == job_id) {
        publish_preview(job.get(), text, final);
    }
}

// --- live preview and early decode ------------------------------------------------------

void TranscriptionRuntime::set_streaming(StreamingRuntime* streaming) {
    preview_link_.disconnect();
    preview_throttle_.reset();
    streaming_ = streaming;
    if (streaming == nullptr) {
        return;
    }
    std::weak_ptr<int> alive = alive_;
    preview_throttle_ = std::make_unique<appcore::PreviewThrottle>(
        streaming->clock(), ui_, [this, alive](std::string job, std::string text, bool final) {
            if (alive.lock()) {
                publish_preview(std::string_view(job), text, final);
            }
        });
    // controller._publish_preview: the preview is the recording's job's.
    preview_link_ =
        streaming->streaming_text_update.connect(ui_, [this](std::string text, bool final) {
            const auto job = slot_.recording_job();
            preview_throttle_->offer(job ? job->id : std::string(), std::move(text), final);
        });
}

void TranscriptionRuntime::discard_early_decode() {
    incremental_->discard();
}

void TranscriptionRuntime::start_early_decode(const DictationJob& job, const Json& settings) {
    RecognitionOptions options;
    std::vector<std::string> phrases;
    if (job.recognition) {
        options.language = job.recognition->language;
        phrases = job.recognition->phrases;
    }
    try {
        incremental_->start(recorder_.tape(), options, speech::engine(settings),
                            recorder_.output_file(), std::move(phrases));
    } catch (const std::exception& error) {
        kLog.error_with(error, "Incremental dictation could not start");
    }
}

void TranscriptionRuntime::stop_preview() {
    StreamingRuntime* streaming = streaming_.load();
    // The window preview returns at once and keeps its unfinished window for the fallback.
    std::string text = streaming != nullptr ? streaming->stop_session() : std::string();
    std::lock_guard lock(state_mutex_);
    pending_streaming_text_ = std::move(text);
}

void TranscriptionRuntime::complete_preview_fallback(const std::filesystem::path& audio_path,
                                                     const std::string& raw) {
    // on_transcription_complete reads the preview only for an empty final transcript, so the
    // stop left its last partial window undecoded; it is decoded here, on this worker, for just
    // that case. Uploads and retranscriptions never stopped a preview of their own: their
    // pending audio path is empty.
    if (!blank(raw) || slot_.cancel_requested()) {
        return;
    }
    {
        std::lock_guard lock(state_mutex_);
        if (pending_.audio_path != audio_path) {
            return;
        }
    }
    StreamingRuntime* streaming = streaming_.load();
    if (streaming == nullptr) {
        return;
    }
    const std::optional<std::string> text = streaming->finalize();
    if (text && !text->empty()) {
        std::lock_guard lock(state_mutex_);
        pending_streaming_text_ = *text;
    }
}

// --- the slot ---------------------------------------------------------------------------

bool TranscriptionRuntime::claim_job(std::shared_ptr<const DictationJob> job,
                                     bool require_idle_recorder) {
    const ActivitySnapshot activity = activity_.snapshot();
    ClaimGates gates;
    gates.shutting_down = activity.shutting_down;
    gates.backup_in_progress = activity.backup_in_progress;
    gates.host_update_active = activity.host_update_active;
    gates.recorder_busy = require_idle_recorder && recorder_.is_recording();
    const bool walkthrough = job && job->mode == JobMode::Walkthrough;
    if (!slot_.claim(std::move(job), gates)) {
        return false;
    }
    activity_.set_transcribing(true);
    // The walkthrough library waits for a walkthrough being transcribed.
    activity_.set_walkthrough_job(walkthrough);
    return true;
}

void TranscriptionRuntime::finish_job() {
    if (const auto job = slot_.active_job(); job && job->mode == JobMode::Walkthrough) {
        end_walkthrough(true);
    }
    std::optional<double> cancel_s;
    {
        std::lock_guard lock(state_mutex_);
        stop_started_.reset();
        if (cancel_started_) {
            cancel_s = seconds_since(*cancel_started_);
            cancel_started_.reset();
        }
        delivery_html_.clear();
        spoken_text_.clear();
        stats_counts_ = Json::object();
        last_cleanup_failure_.reset();
        recording_profile_.reset();
        profile_settings_.reset();
        formatting_profile_.reset();
        refused_ = false;
        filter_canceled_ = false;
        route_failed_ = false;
        shaping_notice_.clear();
        rewrite_pending_ = false;
        deliver_to_clipboard_ = true;
        retry_entry_id_.reset();
        retranscription_ = false;
        remote_retry_exhausted_ = false;
    }
    if (cancel_s) {
        metrics::record("cancel_s", *cancel_s);
    }
    upload_job_ = false;
    // A tape never ends on its own: the early decode of a job that ends here (transcribed,
    // failed or refused) must not keep following it.
    incremental_->discard();
    slot_.finish();
    activity_.set_transcribing(false);
    activity_.set_walkthrough_job(false);
}

void TranscriptionRuntime::forget_recording() {
    slot_.forget_recording();
    {
        // No later job inherits the recording's profile.
        std::lock_guard lock(state_mutex_);
        recording_profile_.reset();
        profile_settings_.reset();
    }
    // Nothing will transcribe the recording: its early decode stops following the tape.
    incremental_->discard();
    // A stop that failed leaves no transcript, but the screenshots stay.
    end_walkthrough(true);
}

void TranscriptionRuntime::begin_shutdown() {
    slot_.cancel();
    std::shared_ptr<CancelSource> write_up;
    {
        std::lock_guard lock(state_mutex_);
        write_up = write_up_cancel_;
    }
    if (write_up) {
        write_up->cancel();
    }
    incremental_->discard();
}

std::string TranscriptionRuntime::report_busy(std::string_view action) {
    const std::string message = busy_message(action, activity_.snapshot().host_update_active);
    status_update.emit(message);
    kLog.info("{}", message);
    return message;
}

void TranscriptionRuntime::clear_pending_audio_metadata() {
    std::lock_guard lock(state_mutex_);
    pending_ = PendingAudio{};
    pending_streaming_text_.clear();
}

// --- start ------------------------------------------------------------------------------

bool TranscriptionRuntime::can_start_recording() {
    const ActivitySnapshot activity = activity_.snapshot();
    if (activity.backup_in_progress) {
        status_update.emit("Wait for backup or restore preparation to finish");
        return false;
    }
    if (activity.host_update_active) {
        notify(std::format("{} is preparing an update. Try again shortly.", identity().product));
        return false;
    }
    if (activity.meeting_active) {
        status_update.emit("Meeting Mode is active — end the meeting to use dictation");
        return false;
    }
    if (services_.hooks.readiness_message) {
        if (const auto message = services_.hooks.readiness_message();
            message && !message->empty()) {
            status_update.emit(*message);
            return false;
        }
    }
    if (has_active_job()) {
        // The status line is out of sight when the window is in the tray, so the pointer
        // notice is what the user sees.
        notify(report_busy("starting another recording"));
        return false;
    }
    return !recorder_.is_recording();
}

bool TranscriptionRuntime::start_recording(StartOptions options) {
    if (!can_start_recording()) {
        return false;
    }
    const Json settings = settings_snapshot();
    std::string route = options.route;
    std::string profile_id = options.profile_id;
    // _recording_format: the action is the one resolved for this capture, whatever the settings
    // say later.
    std::optional<RecordingAction> action;
    if (options.mode == JobMode::Action) {
        if (!options.action_id.empty()) {
            action = automation()->find_action(settings, options.action_id);
        }
        if (!action || !action->enabled) {
            status_update.emit("That action no longer exists");
            return false;
        }
        // The action's own shaping decides the format.
        profile_id = action->profile_id();
    } else if (options.mode == JobMode::Dictation && profile_id.empty() && route.empty()) {
        route = dictation_route(settings);
    }
    std::optional<text::CleanupProfile> profile;
    if (!profile_id.empty()) {
        profile = text::find_cleanup_profile(settings, profile_id);
        if (!profile) {
            status_update.emit("Cleanup profile no longer exists");
            return false;
        }
    }
    try {
        // The output device and a recording would fight over audio.
        if (services_.hooks.stop_playback) {
            services_.hooks.stop_playback();
        } else {
            audio::stop_playback();
        }
    } catch (...) {
        kLog.warning("Could not stop playback before recording");
    }
    if (!recorder_.start_recording()) {
        report_start_failure();
        return false;
    }
    {
        // Taken before the recording is announced: edits and other shortcuts cannot replace the
        // format of a recording already in progress.
        std::lock_guard lock(state_mutex_);
        recording_profile_ = profile;
        profile_settings_.reset();
        if (profile) {
            profile_settings_ = settings;
        }
    }
    // Before anything reads the job, so the focus capture runs while the user speaks.
    std::shared_ptr<const DictationJob> job;
    const std::string action_id = action ? action_key(*action, options.action_id) : std::string();
    try {
        JobOptions job_options;
        job_options.selection = options.selection;
        job_options.action_id = action_id;
        job_options.source = options.source;
        job_options.route = route;
        // begin_job reads the selection only for a job the user started.
        job_options.include_selection = action && reads_selection(*action);
        job = std::make_shared<const DictationJob>(
            begin_job(options.mode, settings, recognition_for(nullptr, settings),
                      std::move(job_options), services_.capture));
    } catch (...) {
        kLog.warning("Dictation job context unavailable; using a bare job");
        DictationJob bare;
        bare.mode = options.mode;
        bare.selection = options.selection;
        bare.action_id = action_id;
        bare.source = options.source;
        bare.route = route;
        job = std::make_shared<const DictationJob>(std::move(bare));
    }
    slot_.set_recording_job(job);
    // The API engine starts its upload's client here, which takes over a second; the recording
    // never waits for it.
    if (services_.hooks.prepare_for_transcription) {
        try {
            services_.hooks.prepare_for_transcription();
        } catch (const std::exception& error) {
            kLog.error_with(error, "Could not prepare the speech engine for this recording");
        } catch (...) {
            kLog.error("Could not prepare the speech engine for this recording");
        }
    }
    // Before the preview can show the overlay, from the action resolved for this capture.
    recording_destination_changed.emit(
        recording_destination(action ? &*action : nullptr, options.mode));
    kLog.info("Recording started");
    clear_stats.emit();
    clear_partial_transcription.emit();
    if (StreamingRuntime* streaming = streaming_.load()) {
        streaming->start_session(job->id);
    }
    if (options.mode != JobMode::Walkthrough) {
        // A walkthrough is decoded section by section once it stops, never as one recording, so
        // early windows would be wasted.
        start_early_decode(*job, settings);
    }
    announce_recording(true);
    events_.publish(events::RECORDING_STARTED, events::recording_started(job.get()));
    slot_.start_preview(job);
    show_listening(options.mode,
                   action ? std::optional<std::string_view>(action->name) : std::nullopt,
                   profile ? std::optional<std::string_view>(profile->name) : std::nullopt);
    // Auto-paste keeps the user's clipboard to put it back; the copy is taken while the user
    // speaks instead of in front of the paste. Command Mode reads the selection through the
    // clipboard first, so its snapshot would no longer be the user's.
    if (options.mode == JobMode::Dictation) {
        const Json now = settings_snapshot();
        if (keys::auto_paste.read(now) && keys::restore_clipboard.read(now)) {
            services_.delivery.clipboard->request_prefetch();
        }
    }
    return true;
}

void TranscriptionRuntime::show_listening(JobMode mode, std::optional<std::string_view> action_name,
                                          std::optional<std::string_view> profile_name) {
    const Listening listening = listening_status(mode, action_name, profile_name);
    overlay_state_update.emit(listening.overlay);
    status_update.emit(listening.status);
}

void TranscriptionRuntime::report_start_failure() {
    slot_.set_recording_job(nullptr);
    std::string reason = recorder_.last_start_error();
    if (reason.empty()) {
        reason = "Could not open the audio stream";
    }
    kLog.error("Failed to start recording: {}", reason);
    services_.delivery.clipboard->discard_prefetch();
    announce_recording(false);
    overlay_state_update.emit(appcore::OverlayState::None);
    status_update.emit(std::format("Failed to start recording: {}", reason));
}

bool TranscriptionRuntime::toggle_recording() {
    const bool recording = recorder_.is_recording();
    kLog.info("Toggle recording. Current state: {}", recording ? "True" : "False");
    if (!recording) {
        return start_recording();
    }
    stop_recording();
    return true;
}

// --- stop -------------------------------------------------------------------------------

void TranscriptionRuntime::stop_recording() {
    if (recorder_.capture_canceled()) {
        // A cancel already discarded this capture and the stream is only closing; a stop now
        // (the record hotkey, or a push-and-hold release) must not claim a job and transcribe
        // what is left.
        kLog.info("Stop ignored: this recording was canceled");
        return;
    }
    const auto walkthrough = walkthrough_session();
    if (walkthrough) {
        // No screenshot may land after the audio it belongs in has ended.
        walkthrough->close();
    }
    StreamingRuntime* streaming = streaming_.load();
    if (streaming != nullptr && streaming->enabled()) {
        // The preview overlay goes at once, so the waveform's Processing and Transcribing states
        // are the only UI after the stop.
        streaming->streaming_overlay_hide.emit();
    }
    {
        std::lock_guard lock(state_mutex_);
        pending_streaming_text_.clear();
    }
    if (streaming != nullptr) {
        streaming->begin_stop();
    }
    if (!recorder_.stop_recording()) {
        forget_recording();
        if (walkthrough) {
            // The walkthrough's column and controls follow the recording state.
            announce_recording(false);
        }
        overlay_state_update.emit(appcore::OverlayState::None);
        status_update.emit("Failed to stop recording");
        return;
    }
    announce_recording(false);
    overlay_state_update.emit(appcore::OverlayState::Processing);
    status_update.emit("Processing...");
    {
        std::lock_guard lock(state_mutex_);
        stop_started_ = clock_.steady();
    }
    std::optional<double> duration;
    try {
        duration = recorder_.get_recording_duration();
    } catch (...) {
        kLog.warning("Recording duration unavailable");
    }
    slot_.end_preview();
    const auto job = slot_.recording_job();
    events_.publish(events::RECORDING_STOPPED, events::recording_stopped(job.get(), duration));
    // Reserved before the post-roll and the save: once the recorder stops, a retry or an upload
    // could otherwise take the slot and have its metadata overwritten here.
    if (!claim_job(job)) {
        // Nothing will transcribe it, so for listeners it ends here.
        events_.publish(events::RECORDING_CANCELED, events::recording_canceled(job.get()));
        forget_recording();
        if (streaming != nullptr) {
            // Deliberately unlike OpenWhisper, whose preview stays "streaming" here: no worker
            // will stop this preview, and a window preview still following the old tape would
            // refuse the next recording's start and leave its preview overlay empty.
            streaming->stop_session();
        }
        report_busy("processing this recording");
        overlay_state_update.emit(appcore::OverlayState::None);
        return;
    }
    // The post-roll keeps capturing for up to 1.2 s and the WAV write follows; waiting here
    // would freeze the UI while the Processing overlay should animate. The claimed slot keeps
    // other jobs out meanwhile.
    post_jobs([this] { finish_recording_job(); });
}

void TranscriptionRuntime::finish_recording_job() {
    try {
        bool stop_completed = false;
        try {
            stop_completed = recorder_.wait_for_stop_completion();
        } catch (...) {
            stop_preview();
            throw;
        }
        // After the post-roll, which the preview keeps for the fallback; the final decode
        // starts at once.
        stop_preview();
        if (slot_.cancel_requested()) {
            // Cancel landed during the post-roll: the capture was discarded, and transcribing
            // what is left would paste audio the user had just thrown away.
            abandon_canceled_job("during post-roll");
            return;
        }
        const auto file_size = saved_recording_size(stop_completed);
        if (!file_size) {
            return;
        }
        const auto job = slot_.active_job();
        const double duration = recorder_.get_recording_duration();
        std::string source_name = recording_source_name(job.get());
        {
            std::lock_guard lock(state_mutex_);
            pending_.audio_path = recorder_.output_file();
            pending_.audio_duration = duration;
            pending_.file_size = *file_size;
            pending_.source_name = std::move(source_name);
        }
        const std::string capture_error = recorder_.last_capture_error();
        if (!capture_error.empty()) {
            report_failure(capture_error);
            return;
        }
        if (slot_.cancel_requested()) {
            // Cancel landed while the WAV saved, when no engine was running to interrupt.
            abandon_canceled_job("before transcription");
            return;
        }
        kLog.info("Transcription started. Duration: {:.2f}s", recorder_.get_recording_duration());
        run_transcription_job(recorder_.output_file());
    } catch (const std::exception& error) {
        kLog.error_with(error, "Failed to start transcription");
        report_failure(start_failure_message(error));
    }
}

std::optional<std::int64_t> TranscriptionRuntime::saved_recording_size(bool stop_completed) {
    const auto failed_with = [this](std::string_view fallback) {
        std::string error = recorder_.last_capture_error();
        report_failure(error.empty() ? std::string(fallback) : std::move(error));
    };
    if (!stop_completed) {
        const std::string message =
            "Recording did not finish stopping; captured audio was kept for recovery.";
        recorder_.fail_capture(message);
        report_failure(message);
        return std::nullopt;
    }
    if (!recorder_.has_recording_data()) {
        kLog.error("No recording data available");
        failed_with("No audio data recorded");
        return std::nullopt;
    }
    if (!recorder_.save_recording()) {
        kLog.error("Failed to save recording");
        failed_with("Failed to save audio file; recovery copy kept");
        return std::nullopt;
    }
    const std::filesystem::path& file = recorder_.output_file();
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        kLog.error("Audio file not found");
        report_failure("Audio file not created");
        return std::nullopt;
    }
    const auto size = static_cast<std::int64_t>(std::filesystem::file_size(file));
    kLog.info("Audio file size: {} bytes", size);
    if (size < static_cast<std::int64_t>(MIN_RECORDING_BYTES)) {
        kLog.error("Audio file too small: {} bytes", size);
        report_failure("Audio file is empty or corrupted");
        return std::nullopt;
    }
    return size;
}

std::string TranscriptionRuntime::recording_source_name(const DictationJob* job) const {
    if (job != nullptr && job->mode == JobMode::Command) {
        return "Command Mode";
    }
    if (job != nullptr && job->mode == JobMode::Action) {
        return action_source_name(*job);
    }
    if (job != nullptr && job->mode == JobMode::Walkthrough) {
        return std::string(WALKTHROUGH_LABEL);
    }
    std::lock_guard lock(state_mutex_);
    if (recording_profile_) {
        return std::format("Quick Record · {}", recording_profile_->name);
    }
    return "Quick Record";
}

void TranscriptionRuntime::report_failure(std::string message) {
    post_ui([this, message = std::move(message)] { on_transcription_error(message); });
}

void TranscriptionRuntime::abandon_canceled_job(std::string_view stage) {
    kLog.info("Dictation canceled {}; nothing transcribed", stage);
    const auto job = slot_.active_job();
    events_.publish(events::TRANSCRIPTION_FAILED,
                    events::transcription_failed(job.get(), events::FAILURE_CANCELED));
    incremental_->discard();
    services_.delivery.clipboard->discard_prefetch();
    end_walkthrough(false);
    clear_pending_audio_metadata();
    finish_job();
}

void TranscriptionRuntime::raise_if_canceled() const {
    if (slot_.cancel_requested()) {
        throw std::runtime_error(std::string(TRANSCRIPTION_CANCELED));
    }
}

// --- decode -----------------------------------------------------------------------------

void TranscriptionRuntime::run_transcription_job(const std::filesystem::path& audio_path) {
    try {
        raise_if_canceled();
        const auto job = slot_.active_job();
        const auto state = speech_.readiness(speech::Slot::Dictation).state;
        if (speech::engine(settings_snapshot()) == "remote" && state != speech::ReadyState::Ready &&
            state != speech::ReadyState::Warming && (!job || job->mode == JobMode::Dictation)) {
            retry_remote_for_recording();
        } else {
            require_backend_ready();
        }
        raise_if_canceled();
        transcribe_audio_file(audio_path);
    } catch (const std::exception& error) {
        kLog.error_with(error, "Failed to start transcription");
        report_failure(start_failure_message(error));
    }
}

void TranscriptionRuntime::require_backend_ready() {
    const speech::Readiness readiness = speech_.readiness(speech::Slot::Dictation);
    // A warming engine takes jobs: its warm-up decode never holds a final pass back.
    if (readiness.state == speech::ReadyState::Ready ||
        readiness.state == speech::ReadyState::Warming) {
        return;
    }
    const std::string engine = speech::engine(settings_snapshot());
    if (is_local_engine(engine)) {
        if (engine == "remote") {
            // A recording's reconnect is retry_remote_for_recording; other jobs only fail.
        } else if (readiness.state == speech::ReadyState::ModelMissing) {
            request_model_consent();
        } else if (services_.hooks.request_reload) {
            services_.hooks.request_reload();
        }
        throw EngineNotReadyError(readiness.device_info);
    }
    if (readiness.state == speech::ReadyState::ModelMissing) {
        // Never transcribe with a model the user has not approved downloading.
        request_model_consent();
        throw EngineNotReadyError(
            "The speech model isn't downloaded yet. Approve the download, then try again.");
    }
}

void TranscriptionRuntime::set_remote_link(std::function<void()> retry_now,
                                           std::function<bool()> restoring) {
    std::lock_guard lock(state_mutex_);
    retry_remote_ = std::move(retry_now);
    remote_restoring_ = std::move(restoring);
}

void TranscriptionRuntime::set_remote_timing(RemoteTimingStart start) {
    std::lock_guard lock(state_mutex_);
    remote_timing_ = std::move(start);
}

void TranscriptionRuntime::retry_remote_for_recording() {
    const auto connected = [this] {
        const auto state = speech_.readiness(speech::Slot::Dictation).state;
        return state == speech::ReadyState::Ready || state == speech::ReadyState::Warming;
    };
    const auto device_info = [this] {
        return speech_.readiness(speech::Slot::Dictation).device_info;
    };
    std::function<void()> retry_now;
    std::function<bool()> restoring_now;
    {
        std::lock_guard lock(state_mutex_);
        retry_now = retry_remote_;
        restoring_now = remote_restoring_;
    }
    if (!retry_now) {
        throw EngineNotReadyError(device_info());
    }
    // Focus may move while the engine is down: the words go to the clipboard's copy button.
    {
        std::lock_guard lock(state_mutex_);
        deliver_to_clipboard_ = false;
    }
    services_.delivery.clipboard->discard_prefetch();
    const CancelToken cancel = slot_.cancel_token();
    const auto deadline = std::chrono::steady_clock::now() + REMOTE_RECOVERY_WAIT;
    // Rounded up: a truncated remainder reads 0 while the deadline is still ahead, and the loop
    // below would start the attempts left without waiting for any of them.
    const auto remaining = [deadline] {
        return std::max(std::chrono::milliseconds(0),
                        std::chrono::ceil<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now()));
    };
    int attempts = 0;
    for (const auto delay : REMOTE_RECOVERY_DELAYS) {
        cancel.wait_for(std::min(delay, remaining()));
        raise_if_canceled();
        if (connected()) {
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        ++attempts;
        const std::string progress =
            std::format("Retrying connection ({}/{})...", attempts, REMOTE_RECOVERY_DELAYS.size());
        transcription_retry_progress.emit(progress);
        status_update.emit(progress);
        retry_now();
        // Settled once the reconnect it started has ended, or the engine is back. One that never
        // starts (another load took over) settles after a second.
        const auto asked = std::chrono::steady_clock::now();
        bool started = false;
        while (!connected() && remaining() > std::chrono::milliseconds(0)) {
            const bool restoring = restoring_now && restoring_now();
            started = started || restoring;
            if (started ? !restoring
                        : std::chrono::steady_clock::now() - asked >= std::chrono::seconds(1)) {
                break;
            }
            cancel.wait_for(std::min(std::chrono::milliseconds(50), remaining()));
            raise_if_canceled();
        }
        if (connected()) {
            return;
        }
    }
    {
        std::lock_guard lock(state_mutex_);
        remote_retry_exhausted_ = true;
    }
    throw EngineNotReadyError(
        std::format("Remote engine unavailable after {} reconnect attempt{}. {}", attempts,
                    attempts == 1 ? "" : "s", device_info()));
}

void TranscriptionRuntime::submit_transcription_job(const std::filesystem::path& audio_path) {
    raise_if_canceled();
    require_backend_ready();
    transcribe_audio_file(audio_path);
}

void TranscriptionRuntime::transcribe_audio_file(std::filesystem::path audio_path) {
    auto job = slot_.active_job();
    try {
        raise_if_canceled();
        bool size_known = false;
        {
            std::lock_guard lock(state_mutex_);
            size_known = pending_.file_size.has_value();
        }
        if (!size_known) {
            const auto size = static_cast<std::int64_t>(std::filesystem::file_size(audio_path));
            std::lock_guard lock(state_mutex_);
            pending_.file_size = size;
        }
        announce_transcription(audio_path);
        std::optional<RecognitionContext> recognition;
        job = with_final_recognition(std::move(job), recognition);
        Completion result = job && job->mode == JobMode::Walkthrough
                                ? complete_walkthrough(audio_path, recognition)
                                : decode_and_complete(audio_path, recognition, job);
        raise_if_canceled();
        post_ui([this, result = std::move(result)] {
            on_transcription_complete(result.text, result.raw_text, result.info);
        });
    } catch (const CommandRefused& refused) {
        refuse(refused.what());
    } catch (const std::exception& error) {
        kLog.error_with(error, "Transcription failed");
        report_failure(error.what());
    }
}

std::shared_ptr<const DictationJob>
TranscriptionRuntime::with_final_recognition(std::shared_ptr<const DictationJob> job,
                                             std::optional<RecognitionContext>& recognition) {
    recognition.reset();
    if (!job) {
        // Uploads and retranscriptions keep the engine's own settings.
        return job;
    }
    try {
        recognition = recognition_for(job.get(), settings_snapshot());
    } catch (...) {
        kLog.warning("Recognition context unavailable; using the engine's own settings");
        return job;
    }
    if (*recognition != job->recognition.value_or(RecognitionContext{})) {
        // The language may have been switched since the recording started; History and Stats
        // record the one decoded with. A slot released meanwhile never gets its job back.
        auto updated = std::make_shared<DictationJob>(*job);
        updated->recognition = *recognition;
        std::shared_ptr<const DictationJob> replacement = updated;
        if (slot_.replace_active_job(job.get(), replacement)) {
            job = std::move(replacement);
        }
    }
    return job;
}

TranscriptionRuntime::Completion
TranscriptionRuntime::decode_and_complete(const std::filesystem::path& audio_path,
                                          const std::optional<RecognitionContext>& recognition,
                                          const std::shared_ptr<const DictationJob>& job) {
    const auto started = clock_.steady();
    {
        std::lock_guard lock(state_mutex_);
        transcription_started_ = started;
    }
    // _timed_decode: windows decoded while recording are not this pass's time, so only the
    // requests from here on count toward the stats line.
    const auto timed_decode = [&] {
        std::function<std::optional<remote::RemoteTiming>()> since;
        if (speech::engine(settings_snapshot()) == "remote") {
            RemoteTimingStart start;
            {
                std::lock_guard lock(state_mutex_);
                start = remote_timing_;
            }
            if (start) {
                since = start();
            }
        }
        std::string text = decode_path(audio_path, recognition);
        if (since) {
            auto timing = since();
            std::lock_guard lock(state_mutex_);
            pending_.remote_timing = std::move(timing);
        }
        return text;
    };
    std::string raw;
    try {
        raw = timed_decode();
    } catch (const EngineError& error) {
        // A paired host that refused the decode before starting it (it was reloading, or the
        // connection had dropped while idle): a recording whose audio is kept joins the
        // reconnects, then sends it again (_decode_and_complete's RemoteDecodeNotStarted).
        bool saved_audio = false;
        {
            std::lock_guard lock(state_mutex_);
            saved_audio = pending_.audio_path.has_value() || retry_entry_id_.has_value();
        }
        if (error.retry != EngineError::Retry::Connection ||
            speech::engine(settings_snapshot()) != "remote" || !saved_audio ||
            (job && job->mode != JobMode::Dictation) || slot_.cancel_requested()) {
            throw;
        }
        kLog.info("The remote host did not start the decode; reconnecting");
        retry_remote_for_recording();
        raise_if_canceled();
        announce_transcription(audio_path);
        raw = timed_decode();
    }
    {
        std::lock_guard lock(state_mutex_);
        // Only the final pass counts toward the stats line.
        pending_.transcription_time = seconds_since(started);
        transcription_started_.reset();
    }
    complete_preview_fallback(audio_path, raw);
    raise_if_canceled();
    if (job && (job->mode == JobMode::Command || job->mode == JobMode::Transform)) {
        // _complete_command.
        CommandCompletion* command = command_.load();
        if (command == nullptr) {
            throw std::runtime_error("Command Mode isn't available");
        }
        RewriteResult result = command->complete_recording(raw, job);
        return Completion{std::move(result.text), std::move(result.raw_text),
                          std::move(result.info)};
    }
    if (job && (job->mode == JobMode::Dictation || job->mode == JobMode::Action)) {
        return complete_live(raw, job);
    }
    return maybe_cleanup_transcript(raw);
}

struct TranscriptionRuntime::CleanedSections {
    // The finished words of each section, and the same before AI cleanup.
    std::vector<std::string> cleaned;
    std::vector<std::string> raw;
    // The first section's cleanup that ran, or none.
    std::optional<CleanupInfo> info;
};

TranscriptionRuntime::Completion
TranscriptionRuntime::complete_walkthrough(const std::filesystem::path& audio_path,
                                           const std::optional<RecognitionContext>& recognition) {
    // The document waits on the session for deliver_walkthrough.
    const auto session = walkthrough_session();
    if (!session) {
        throw std::runtime_error("The walkthrough's screenshots are missing");
    }
    try {
        session->preserve_audio(audio_path);
    } catch (const std::system_error& error) {
        // Python's OSError: only the recovery copy is lost; the walkthrough goes on without it.
        kLog.error_with(error, "Could not keep the walkthrough's recovery audio");
    }
    const walkthrough::Storyboard board = session->storyboard();
    const std::vector<walkthrough::Shot> shots = board.shots;
    const auto started = clock_.steady();
    {
        std::lock_guard lock(state_mutex_);
        transcription_started_ = started;
    }
    // Each section is decoded as a file of its own, as a dictation's recording is.
    auto [texts, notes] = walkthrough::transcribe_sections(
        audio_path, board,
        [this, &recognition](const std::filesystem::path& clip) {
            return decode_path(clip, recognition);
        },
        [this](int index, int total) { report_section(index, total); });
    {
        std::lock_guard lock(state_mutex_);
        pending_.transcription_time = seconds_since(started);
        transcription_started_.reset();
    }
    raise_if_canceled();
    CleanedSections sections = clean_sections(texts);
    {
        std::lock_guard lock(state_mutex_);
        stats_counts_ = Json{{"shots", static_cast<int>(shots.size())}};
    }
    raise_if_canceled();
    const bool spoken =
        std::ranges::any_of(sections.cleaned, [](const std::string& text) { return !blank(text); });
    if (shots.empty() && !spoken) {
        // Nothing was said and nothing was kept: no document to write. It ends like an empty
        // dictation.
        return Completion{std::string(), std::nullopt, sections.info};
    }
    status_update.emit("Writing the walkthrough...");
    double duration_s = 0.0;
    {
        std::lock_guard lock(state_mutex_);
        duration_s = pending_.audio_duration.value_or(0.0);
    }
    const auto keep_words = [&](std::string error) {
        // A full disk or a read-only folder loses the document, never the words: they still
        // reach History and the clipboard.
        session->set_document_error(std::move(error));
        const int count = static_cast<int>(shots.size());
        std::string text = walkthrough::marked_text(sections.cleaned, count);
        std::string raw = walkthrough::marked_text(sections.raw, count);
        std::optional<std::string> raw_text;
        if (raw != text) {
            raw_text = std::move(raw);
        }
        return Completion{std::move(text), std::move(raw_text), sections.info};
    };
    walkthrough::WalkthroughResult result;
    try {
        result = walkthrough::build_documents(*session, shots, sections.cleaned, sections.raw,
                                              duration_s, notes, settings_snapshot());
    } catch (const walkthrough::DocumentError& error) {
        kLog.error_with(error, "Could not write the walkthrough document");
        return keep_words(error.what());
    } catch (const std::system_error& error) {
        // Python's OSError: its strerror, else the whole message.
        kLog.error_with(error, "Could not write the walkthrough document");
        std::string reason = strerror_text(error.code());
        return keep_words(reason.empty() ? std::string(error.what()) : std::move(reason));
    }
    if (const int lost = session->lost_shots(); lost != 0) {
        result.lost_shots = lost;
    }
    std::vector<std::filesystem::path> shot_paths;
    shot_paths.reserve(shots.size());
    for (const auto& shot : shots) {
        shot_paths.push_back(shot.path);
    }
    session->set_result(result);
    session->set_write_up_input({std::move(shot_paths), std::move(notes)});
    return Completion{result.history_text, result.history_raw, sections.info};
}

TranscriptionRuntime::CleanedSections
TranscriptionRuntime::clean_sections(const std::vector<std::string>& texts) {
    // Each section on its own, so no sentence moves across a screenshot.
    CleanedSections sections;
    for (const std::string& text : texts) {
        raise_if_canceled();
        Completion fixed = maybe_cleanup_transcript(text);
        sections.raw.push_back(fixed.raw_text ? *fixed.raw_text : fixed.text);
        sections.cleaned.push_back(std::move(fixed.text));
        if (!sections.info) {
            sections.info = std::move(fixed.info);
        }
    }
    return sections;
}

void TranscriptionRuntime::report_section(int index, int total) {
    raise_if_canceled();
    if (total > 1) {
        status_update.emit(std::format("Transcribing section {} of {}...", index, total));
    }
}

TranscriptionRuntime::Completion
TranscriptionRuntime::maybe_cleanup_transcript(const std::string& raw,
                                               const std::optional<std::string>& batch_context,
                                               std::optional<double> timeout_s) {
    CleanupStageInput input;
    // A profile without its start snapshot reads fresh settings, as everything else does.
    bool snapshot_taken = false;
    {
        std::lock_guard lock(state_mutex_);
        // A recording's profile keeps the settings it started with.
        input.profile = recording_profile_;
        if (recording_profile_ && profile_settings_) {
            input.settings = *profile_settings_;
            snapshot_taken = true;
        }
    }
    if (!snapshot_taken) {
        input.settings = settings_snapshot();
    }
    input.batch_context = batch_context;
    input.timeout_s = timeout_s;
    return clean(raw, std::move(input));
}

TranscriptionRuntime::Completion
TranscriptionRuntime::cleanup_as(const std::string& raw,
                                 std::optional<text::CleanupProfile> profile, bool allow_ai) {
    CleanupStageInput input;
    input.profile = std::move(profile);
    input.settings = settings_snapshot();
    input.allow_ai = allow_ai;
    return clean(raw, std::move(input));
}

TranscriptionRuntime::Completion TranscriptionRuntime::clean(const std::string& raw,
                                                             CleanupStageInput input) {
    {
        std::lock_guard lock(state_mutex_);
        last_cleanup_failure_.reset();
        delivery_html_.clear();
        spoken_text_.clear();
        stats_counts_ = Json::object();
        // Words shaped "raw" never reach a formatting pass, so the last one stays.
        if (input.allow_ai) {
            formatting_profile_ = input.profile;
        }
    }
    const auto job = slot_.active_job();
    input.job = job.get();
    input.cancel = slot_.cancel_token();

    const auto pass = cleaner();
    CleanupStageServices services;
    services.preparer = &preparer();
    services.cleaner = pass.get();
    services.clock = &clock_;
    services.announce = [this](appcore::OverlayState state, const std::string& status) {
        overlay_state_update.emit(state);
        status_update.emit(status);
    };
    CleanupOutcome outcome = run_cleanup(raw, input, services);
    {
        std::lock_guard lock(state_mutex_);
        last_cleanup_failure_ = outcome.failure;
        delivery_html_ = outcome.delivery_html;
        spoken_text_ = outcome.spoken_text;
        stats_counts_ = outcome.stats_counts;
    }
    return Completion{std::move(outcome.text), std::move(outcome.raw_text),
                      std::move(outcome.info)};
}

// --- automation -------------------------------------------------------------------------

void TranscriptionRuntime::set_automation(AutomationPort& automation) {
    set_automation(std::shared_ptr<AutomationPort>(&automation, [](AutomationPort*) {}));
}

void TranscriptionRuntime::set_automation(std::shared_ptr<AutomationPort> automation) {
    automation_.store(std::move(automation));
}

std::shared_ptr<AutomationPort> TranscriptionRuntime::automation() const {
    return automation_.load();
}

TranscriptionRuntime::Completion
TranscriptionRuntime::complete_live(const std::string& raw,
                                    std::shared_ptr<const DictationJob> job) {
    // With no filters, no routing and no action this is exactly a dictation's cleanup.
    const Json settings = settings_snapshot();
    const std::optional<std::string> text = job->mode == JobMode::Dictation && !job->route.empty()
                                                ? filter_and_route(raw, job, settings)
                                                : run_filters(BEFORE_CLEANUP, raw, job, settings);
    if (!text) {
        return Completion{raw, std::nullopt, std::nullopt};
    }
    // A Cancel during any stage ends the job here: canceled words never reach the next filter,
    // the judge or cleanup.
    raise_if_canceled();
    Completion done = job->mode == JobMode::Action ? shape_action(*text, *job, settings)
                      : job->route == "hotkey" && automation()->raw_before_consent()
                          ? cleanup_as(*text, std::nullopt, false)
                          : maybe_cleanup_transcript(*text);
    if (blank(done.text)) {
        return done;
    }
    raise_if_canceled();
    const std::optional<std::string> final = run_filters(AFTER_CLEANUP, done.text, job, settings);
    if (!final || *final == done.text) {
        return done;
    }
    {
        // The rich-text version no longer matches the words.
        std::lock_guard lock(state_mutex_);
        delivery_html_.clear();
    }
    std::optional<std::string> raw_text = done.raw_text ? std::move(done.raw_text) : done.text;
    return Completion{*final, std::move(raw_text), std::move(done.info)};
}

std::optional<std::string>
TranscriptionRuntime::filtered(const FilterCall& run, const std::string& text,
                               std::shared_ptr<const DictationJob>& job) {
    FilterResult outcome;
    try {
        outcome = run(text, *job);
    } catch (const std::exception& error) {
        // Type only: a filter's error may quote the dictated text.
        kLog.warning("Text filters failed ({}); keeping the text", error_kind(error));
        return text;
    } catch (...) {
        kLog.warning("Text filters failed (Exception); keeping the text");
        return text;
    }
    // A filter takes over by canceling or by turning the words into nothing; a transcript that
    // was empty to begin with is no speech, not that.
    const bool emptied = !blank(text) && blank(outcome.text);
    if (outcome.canceled || emptied) {
        std::lock_guard lock(state_mutex_);
        filter_canceled_ = true;
        return std::nullopt;
    }
    if (outcome.program_changed) {
        auto changed = std::make_shared<DictationJob>(*job);
        changed->changed_by_program = true;
        std::shared_ptr<const DictationJob> replacement = changed;
        slot_.replace_active_job(job.get(), replacement);
        job = std::move(replacement);
    }
    return std::move(outcome.text);
}

std::optional<std::string>
TranscriptionRuntime::run_filters(std::string_view stage, const std::string& text,
                                  std::shared_ptr<const DictationJob>& job, const Json& settings) {
    std::unique_ptr<FilterStageRun> run;
    try {
        run = automation()->filter_stage(stage, settings, slot_.cancel_token());
    } catch (const std::exception& error) {
        kLog.warning("Text filters failed ({}); keeping the text", error_kind(error));
        return text;
    }
    if (!run) {
        return text;
    }
    return filtered(
        [&run](std::string words, const DictationJob& current) {
            return run->apply(std::move(words), current);
        },
        text, job);
}

std::optional<std::string> TranscriptionRuntime::filter_and_route(
    const std::string& raw, std::shared_ptr<const DictationJob>& job, const Json& settings) {
    // The user's saved filters, then routing, then the filters programs leased, on the job as
    // routed: a lease never sees words routed to an action that would not take a program's
    // words, and cannot steer where they go. Routing takes none of the stage's budget.
    std::unique_ptr<FilterStageRun> before;
    try {
        before = automation()->filter_stage(BEFORE_CLEANUP, settings, slot_.cancel_token());
    } catch (const std::exception& error) {
        kLog.warning("Text filters failed ({}); keeping the text", error_kind(error));
    }
    std::optional<std::string> text = raw;
    if (before) {
        text = filtered(
            [&before](std::string words, const DictationJob& current) {
                return before->saved(std::move(words), current);
            },
            raw, job);
        if (!text) {
            return std::nullopt;
        }
    }
    raise_if_canceled();
    text = route_words(*text, job, settings);
    raise_if_canceled();
    if (!before) {
        return text;
    }
    return filtered(
        [&before](std::string words, const DictationJob& current) {
            return before->leased(std::move(words), current);
        },
        *text, job);
}

std::string TranscriptionRuntime::route_words(const std::string& text,
                                              std::shared_ptr<const DictationJob>& job,
                                              const Json& settings) {
    // Anything short of a confident answer leaves the dictation as it is; when the judge could
    // not answer, the paste says so.
    auto port = automation();
    std::optional<RouteDecision> decision;
    try {
        decision = port->route(text, settings, job->route, slot_.cancel_token(), job->id);
    } catch (const std::exception& error) {
        // Type only: the judge's error may quote the dictated text.
        kLog.warning("Spoken routing failed ({}); pasting the dictation", error_kind(error));
    } catch (...) {
        kLog.warning("Spoken routing failed (Exception); pasting the dictation");
    }
    if (slot_.cancel_requested())
        return text;
    if (decision && decision->failure == "consent_declined") {
        // The explicit routing request was declined. Cancel the job rather than treating it as
        // a failed judgment and pasting the words into the previously focused application.
        slot_.cancel();
        return text;
    }
    if (decision && !decision->failure.empty()) {
        std::lock_guard lock(state_mutex_);
        route_failed_ = true;
        return text;
    }
    if (!decision || !decision->action_id) {
        return text;
    }
    const std::optional<RecordingAction> action = port->find_action(settings, *decision->action_id);
    if (!action || !action->enabled) {
        return text;
    }
    auto routed = std::make_shared<DictationJob>(*job);
    routed->mode = JobMode::Action;
    routed->action_id = action_key(*action, *decision->action_id);
    routed->routed = true;
    std::shared_ptr<const DictationJob> replacement = routed;
    slot_.replace_active_job(job.get(), replacement);
    job = std::move(replacement);
    {
        std::lock_guard lock(state_mutex_);
        pending_.source_name = std::format("Action · {}", action->name);
    }
    return blank(decision->payload) ? text : decision->payload;
}

TranscriptionRuntime::Completion TranscriptionRuntime::shape_action(const std::string& text,
                                                                    const DictationJob& job,
                                                                    const Json& settings) {
    // Never refuses: the words were dictated, so a shaping problem falls back to the cleaned
    // words, as a failed cleanup falls back to raw text. An action deleted meanwhile still gets
    // them; the action runtime says it is gone and copies them.
    const std::optional<RecordingAction> action =
        automation()->find_action(settings, job.action_id);
    if (!action) {
        // An action can be removed while recording. Its shaping is no longer authorized, so
        // keep the dictated words local rather than falling through to a remote cleanup model.
        return cleanup_as(text, std::nullopt, !automation()->raw_before_consent());
    }
    if (action->shaping == SHAPING_RAW) {
        return cleanup_as(text, std::nullopt, false);
    }
    if (const std::string profile_id = action->profile_id(); !profile_id.empty()) {
        bool recorded = false;
        {
            std::lock_guard lock(state_mutex_);
            recorded = recording_profile_ && recording_profile_->id == profile_id;
        }
        if (recorded) {
            return maybe_cleanup_transcript(text);
        }
        return cleanup_as(text, text::find_cleanup_profile(settings, profile_id));
    }
    Completion done = cleanup_as(text, std::nullopt);
    const std::string transform_id = action->transform_id();
    if (transform_id.empty() || blank(done.text)) {
        return done;
    }
    const std::optional<text::Transform> transform = text::find_transform(settings, transform_id);
    if (!transform) {
        std::lock_guard lock(state_mutex_);
        shaping_notice_ = "This action's transform no longer exists — sent your cleaned words";
        return done;
    }
    overlay_state_update.emit(appcore::OverlayState::Rewriting);
    RewriteResult rewritten;
    try {
        CommandCompletion* command = command_.load();
        if (command == nullptr) {
            throw std::runtime_error("Command Mode isn't available");
        }
        rewritten = command->rewrite_text(done.text, transform->instruction);
    } catch (const std::exception& error) {
        // The cancel ends the rewrite's wait; that is a cancel, not a failure.
        raise_if_canceled();
        kLog.warning("An action's transform couldn't run ({}); sending the cleaned words",
                     error_kind(error));
        std::lock_guard lock(state_mutex_);
        shaping_notice_ = std::format("{} couldn't run — sent your cleaned words", transform->name);
        return done;
    }
    {
        std::lock_guard lock(state_mutex_);
        delivery_html_.clear();
    }
    std::optional<std::string> raw_text = done.raw_text ? std::move(done.raw_text) : done.text;
    std::optional<CleanupInfo> info = rewritten.info ? std::move(rewritten.info) : done.info;
    return Completion{std::move(rewritten.text), std::move(raw_text), std::move(info)};
}

std::string TranscriptionRuntime::action_source_name(const DictationJob& job) const {
    std::optional<RecordingAction> action;
    try {
        action = automation()->find_action(settings_snapshot(), job.action_id);
    } catch (...) {
        kLog.warning("Action name unavailable for History");
    }
    return action ? std::format("Action · {}", action->name) : std::string("Action");
}

std::string TranscriptionRuntime::run_action_text(std::string_view action_id, std::string_view text,
                                                  std::string_view source) {
    std::string words = py::strip(text);
    if (words.empty()) {
        throw RunRefused(RUN_REFUSED, "There are no words to send.");
    }
    if (activity_.snapshot().meeting_active) {
        throw RunRefused(RUN_BUSY, "Meeting Mode is active; end the meeting first.");
    }
    auto port = automation();
    const Json settings = settings_snapshot();
    const std::optional<RecordingAction> action = port->find_action(settings, action_id);
    if (!action || !action->enabled) {
        throw RunRefused(RUN_NOT_FOUND, "No enabled action has that id.");
    }
    if (!user_started(source) && !port->program_words_allowed(*action, settings)) {
        // A program's words would reach the user's coding agent as a prompt; delivery would
        // refuse them, so say so now.
        throw RunPermissionDenied(std::string(keys::api_agent_prompts.key()));
    }
    if (!port->accepting()) {
        throw RunRefused(RUN_BUSY,
                         std::format("{} isn't taking actions right now.", identity().product));
    }
    JobOptions options;
    options.action_id = action_key(*action, action_id);
    options.source = std::string(source);
    options.include_selection = reads_selection(*action);
    auto job = std::make_shared<const DictationJob>(
        begin_job(JobMode::Action, settings, recognition_for(nullptr, settings), std::move(options),
                  services_.capture));
    if (!begin_rewrite_job(job, std::format("Action · {}", action->name))) {
        throw RunRefused(RUN_BUSY,
                         std::format("{} is busy with another recording.", identity().product));
    }
    status_update.emit(std::format("{}...", action->name));
    submit_rewrite([this, words = std::move(words), job] {
        Completion done = complete_live(words, job);
        return RewriteResult{std::move(done.text), std::move(done.raw_text), std::move(done.info)};
    });
    return job->id;
}

std::shared_ptr<TranscriptCleaner> TranscriptionRuntime::cleaner() const {
    if (TranscriptCleaner* set = cleaner_.load()) {
        // Not owned: set_cleaner's caller keeps it alive.
        return std::shared_ptr<TranscriptCleaner>(std::shared_ptr<void>(), set);
    }
    {
        std::lock_guard lock(state_mutex_);
        if (llm_cleaner_) {
            return llm_cleaner_;
        }
    }
    return std::shared_ptr<TranscriptCleaner>(std::shared_ptr<void>(), &null_transcript_cleaner());
}

const TextPreparer& TranscriptionRuntime::preparer() const {
    const TextPreparer* set = preparer_.load();
    return set != nullptr ? *set : dictionary_preparer();
}

void TranscriptionRuntime::request_model_consent() {
    std::function<void()> hook;
    {
        std::lock_guard lock(state_mutex_);
        hook = model_consent_;
    }
    if (hook) {
        hook();
    }
}

// --- results ----------------------------------------------------------------------------

void TranscriptionRuntime::on_transcription_complete(std::string transcript,
                                                     std::optional<std::string> raw_text,
                                                     std::optional<CleanupInfo> cleanup_info) {
    const auto job = slot_.active_job();
    std::optional<double> stop_to_result;
    {
        std::lock_guard lock(state_mutex_);
        if (stop_started_) {
            stop_to_result = seconds_since(*stop_started_);
            stop_started_.reset();
        }
    }
    if (stop_to_result) {
        metrics::record("stop_to_result_s", *stop_to_result);
    }
    // From here the result is delivered and only saving holds the slot.
    if (slot_.settle()) {
        // The cancel arrived between the worker's result and this call.
        on_transcription_error(std::string(TRANSCRIPTION_CANCELED));
        return;
    }
    const bool is_empty = blank(transcript);
    std::string preview;
    PendingAudio pending;
    std::optional<double> transcription_time;
    std::optional<std::string> retry_entry_id;
    bool deliver_to_clipboard = true;
    std::optional<std::string> spoken_text;
    {
        std::lock_guard lock(state_mutex_);
        if (is_empty) {
            preview = py::strip(pending_streaming_text_);
        }
        pending = pending_;
        transcription_time = pending_.transcription_time;
        pending_.transcription_time.reset();
        pending_.remote_timing.reset();
        if (!transcription_time && transcription_started_) {
            transcription_time = seconds_since(*transcription_started_);
        }
        transcription_started_.reset();
        retry_entry_id = retry_entry_id_;
        deliver_to_clipboard = deliver_to_clipboard_;
        if (!spoken_text_.empty()) {
            spoken_text = spoken_text_;
        }
    }
    // A preview heard speech the full pass dropped: shown, never saved or pasted.
    const std::string display_text = is_empty && !preview.empty() ? preview
                                     : is_empty                   ? std::string(EMPTY_ASR_MESSAGE)
                                                                  : transcript;
    transcription_completed.emit(display_text, raw_text, cleanup_info);
    overlay_state_update.emit(appcore::OverlayState::None);

    std::optional<double> cleanup_time;
    if (cleanup_info && cleanup_info->elapsed_s > 0) {
        cleanup_time = cleanup_info->elapsed_s;
    }
    if (transcription_time) {
        transcription_stats.emit(
            TranscriptionStats{*transcription_time, pending.audio_duration.value_or(0.0),
                               pending.file_size.value_or(0), cleanup_time, pending.remote_timing});
    }

    if (is_empty) {
        kLog.info("Empty ASR result; skipping history, clipboard, and paste");
        // Listeners saw the recording start; this is how it ended.
        events_.publish(events::TRANSCRIPTION_FAILED,
                        events::transcription_failed(job.get(), events::FAILURE_NO_SPEECH));
        std::string status =
            std::string(preview.empty() ? EMPTY_ASR_MESSAGE : EMPTY_PREVIEW_FALLBACK_MESSAGE);
        if (retry_entry_id) {
            status += " — recording kept in History";
        }
        status_update.emit(status);
        if (job && job->mode == JobMode::Walkthrough) {
            // The column said "Writing it up…"; nothing is coming.
            if (WalkthroughDeliveryPort* delivery = walkthrough_delivery()) {
                delivery->dismiss_walkthrough_panel();
            }
        }
        services_.delivery.clipboard->discard_prefetch();
        clear_pending_audio_metadata();
        finish_job();
        return;
    }

    QueuedEntry entry;
    entry.fields.text = transcript;
    entry.fields.model = model_info_for_history();
    entry.fields.source_audio_path = pending.audio_path;
    entry.fields.transcription_time = transcription_time;
    entry.fields.audio_duration = pending.audio_duration;
    entry.fields.file_size = pending.file_size;
    entry.fields.raw_text = raw_text;
    if (cleanup_info) {
        entry.fields.cleanup_provider = cleanup_info->provider;
        entry.fields.cleanup_model = cleanup_info->model;
    }
    entry.fields.source_name = pending.source_name;
    // Read by Stats only; History drops them.
    entry.fields.spoken_text = spoken_text;
    entry.fields.stats_counts = entry_stats_counts(job.get());
    // Only a recording's own WAV is dictated live; a retranscription is read from a file.
    const bool live = pending.audio_path.has_value();
    const Json context = history_context(job.get(), cleanup_info, live);
    entry.job_id = job ? job->id : std::string();
    events_.publish(events::TRANSCRIPTION_COMPLETED,
                    events::transcription_completed(job.get(), transcript, raw_text, context,
                                                    cleanup_info, pending.audio_duration));

    std::optional<std::string> cleanup_failure;
    std::optional<std::string> formatting_profile;
    std::string shaping_notice;
    {
        std::lock_guard lock(state_mutex_);
        cleanup_failure = last_cleanup_failure_;
        if (formatting_profile_) {
            formatting_profile = formatting_profile_->name;
        }
        shaping_notice = shaping_notice_;
    }
    // _delivery_notice: an action's shaping problem, else a profile's fallback; standard cleanup
    // falls back silently.
    const std::string notice = delivery_notice(shaping_notice, formatting_profile, cleanup_failure);
    const std::string status_suffix = notice.empty() ? std::string() : " — " + notice;
    if (!deliver_to_clipboard) {
        const std::string status =
            live || retry_entry_id ? "Transcript ready to copy" : std::string(READY);
        status_update.emit(status + status_suffix);
        if (live) {
            notify("Transcript recovered. Ready to copy.", true);
        }
        entry.fields.columns = to_context(context);
        if (retry_entry_id) {
            entry.retry_entry_id = retry_entry_id;
            entry.fields.columns["entry_kind"] = std::string(FILE_ENTRY_KIND);
        }
        queue_history({std::move(entry)});
        return;
    }
    bool filtered_out = false;
    {
        std::lock_guard lock(state_mutex_);
        filtered_out = std::exchange(filter_canceled_, false);
    }
    if (filtered_out) {
        // A text filter took over delivery: the words are saved, never pasted or sent.
        services_.delivery.clipboard->discard_prefetch();
        status_update.emit(std::string(FILTER_CANCELED_MESSAGE));
        entry.fields.columns = to_context(context);
        queue_history({std::move(entry)});
        return;
    }
    // Delivered at once; the retained audio's copy, fsync and the database write run on the
    // persistence strand while the target app stays responsive.
    try {
        deliver(transcript, job, notice, raw_text);
    } catch (...) {
        entry.fields.columns = to_context(context);
        queue_history({std::move(entry)});
        throw;
    }
    entry.fields.columns = to_context(context);
    queue_history({std::move(entry)});
}

void TranscriptionRuntime::on_transcription_error(std::string error_message) {
    {
        std::lock_guard lock(state_mutex_);
        stop_started_.reset();
    }
    const auto job = slot_.active_job();
    // Reported now; keeping its audio is all that holds the slot.
    slot_.settle();
    bool refused = false;
    std::optional<std::string> retry_entry_id;
    PendingAudio pending;
    {
        std::lock_guard lock(state_mutex_);
        refused = refused_;
        refused_ = false;
        retry_entry_id = retry_entry_id_;
        pending = pending_;
    }
    const bool canceled = slot_.cancel_requested();
    const std::string_view category = refused    ? events::FAILURE_REFUSED
                                      : canceled ? events::FAILURE_CANCELED
                                                 : events::FAILURE_FAILED;
    events_.publish(events::TRANSCRIPTION_FAILED,
                    events::transcription_failed(job.get(), category, error_message));
    if (category == events::FAILURE_CANCELED) {
        // A Cancel throws a walkthrough's screenshots away whenever it comes, as it does while
        // recording.
        end_walkthrough(false);
    }
    const JobMode mode = job ? job->mode : JobMode::Dictation;
    std::string status =
        error_status(error_message, refused, mode,
                     mode == JobMode::Walkthrough ? walkthrough_kept_note() : std::string());
    if (retry_entry_id) {
        status += " — recording kept in History";
    }
    status_update.emit(status);
    transcription_failed.emit(status);
    overlay_state_update.emit(appcore::OverlayState::None);
    // A plain dictation's failure has the status line and the error dialog; refusals and
    // rewrites, run from other apps, also show near the pointer, unless canceled.
    const bool rewrite = refused || (job && job->mode != JobMode::Dictation);
    if (rewrite && !slot_.cancel_requested()) {
        notify(status);
    }
    {
        std::lock_guard lock(state_mutex_);
        transcription_started_.reset();
        pending_.transcription_time.reset();
        pending_.remote_timing.reset();
    }
    services_.delivery.clipboard->discard_prefetch();
    if (pending.audio_path) {
        const std::filesystem::path path = *pending.audio_path;
        const bool keep_in_history =
            category == events::FAILURE_FAILED && (!job || job->mode == JobMode::Dictation);
        std::weak_ptr<int> alive = alive_;
        bool exhausted = false;
        {
            std::lock_guard lock(state_mutex_);
            exhausted = remote_retry_exhausted_;
        }
        persistence_.post([this, alive, path, status, pending, keep_in_history, exhausted] {
            if (!alive.lock()) {
                return;
            }
            if (keep_in_history) {
                save_failed_recording(path, status, pending, exhausted);
            } else {
                preserve_failed_audio(path, status);
            }
        });
        return;
    }
    clear_pending_audio_metadata();
    finish_job();
}

void TranscriptionRuntime::on_history_persisted(HistoryOutcome outcome) {
    try {
        history_persisted.emit(outcome);
        if (outcome.refresh) {
            refresh_history.emit();
        }
        if (!outcome.error.empty()) {
            status_update.emit(outcome.error);
        }
        if (!outcome.notice.empty()) {
            bool exhausted = false;
            {
                std::lock_guard lock(state_mutex_);
                exhausted = remote_retry_exhausted_;
            }
            // transcription.py on_history_persisted: after the reconnects ran out the notice is
            // the overlay's even while the main window is in front; any other is a plain notice.
            if (exhausted) {
                transcription_retry_failed.emit(outcome.notice);
            } else {
                notify(outcome.notice);
            }
        }
    } catch (const std::exception& error) {
        kLog.error_with(error, "Could not refresh saved transcription history");
    } catch (...) {
        kLog.error("Could not refresh saved transcription history");
    }
    clear_pending_audio_metadata();
    finish_job();
}

// --- delivery ---------------------------------------------------------------------------

void TranscriptionRuntime::set_delivery_status(const std::string& status) {
    delivery_status_ = status;
    status_update.emit(status);
}

bool TranscriptionRuntime::apply_clipboard_and_paste(const std::string& transcript,
                                                     const std::string& status_suffix,
                                                     const std::string& html, bool force_paste) {
    return dictation::apply_clipboard_and_paste(
        services_.delivery, settings_snapshot(), transcript,
        [this](const std::string& status) { set_delivery_status(status); }, status_suffix, html,
        force_paste);
}

bool TranscriptionRuntime::insert_into_scratchpad(const std::string& transcript,
                                                  const std::string& status_suffix) {
    ScratchpadPort* scratchpad = scratchpad_.load();
    const auto& insert = services_.delivery.insert_into_scratchpad;
    if (scratchpad == nullptr && !insert) {
        return false;
    }
    bool inserted = false;
    try {
        if (scratchpad != nullptr) {
            // Only a Scratchpad that is visible and the active window takes the dictation;
            // otherwise the text goes to the app as usual.
            inserted =
                !transcript.empty() && scratchpad->is_focused() && scratchpad->insert(transcript);
        } else {
            inserted = insert(transcript);
        }
    } catch (const std::exception& error) {
        kLog.error_with(error, "Could not add the transcript to the Scratchpad");
    } catch (...) {
        kLog.error("Could not add the transcript to the Scratchpad");
    }
    if (inserted) {
        services_.delivery.clipboard->discard_prefetch();
        status_update.emit(std::string(ADDED_TO_SCRATCHPAD) + status_suffix);
    }
    return inserted;
}

void TranscriptionRuntime::copy_rewrite(const std::string& transcript, bool app_changed) {
    services_.delivery.clipboard->discard_prefetch();
    const bool copied = services_.delivery.clipboard->write_text(transcript);
    const std::string status =
        rewrite_copied_status(copied, app_changed ? PasteTarget::Changed : PasteTarget::Unknown);
    status_update.emit(status);
    notify(status);
}

void TranscriptionRuntime::notify_forced_outcome(const DictationJob& job, bool pasted) {
    if (const auto notice =
            forced_outcome_notice(pasted, !blank(job.selection_text()), delivery_status_)) {
        notify(notice->text, notice->done);
    }
}

// --- rewrites ---------------------------------------------------------------------------

const TextPreparer& TranscriptionRuntime::text_preparer() const {
    return preparer();
}

void TranscriptionRuntime::set_command_runtime(CommandCompletion* command) {
    command_ = command;
}

bool TranscriptionRuntime::begin_rewrite_job(std::shared_ptr<const DictationJob> job,
                                             std::string source_name) {
    // Python takes its capture lock without waiting here, so a start that holds it while the
    // audio device opens refuses the rewrite; starts, stops and cancels all run on this thread
    // in this runtime, so none can be under way now.
    if (!job) {
        return false;
    }
    if (recorder_.is_recording() || !claim_job(job, true)) {
        refuse_rewrite(*job);
        return false;
    }
    forget_recording();
    {
        std::lock_guard lock(state_mutex_);
        rewrite_pending_ = true;
        deliver_to_clipboard_ = true;
    }
    clear_pending_audio_metadata();
    {
        std::lock_guard lock(state_mutex_);
        pending_.source_name = std::move(source_name);
    }
    overlay_state_update.emit(appcore::OverlayState::Rewriting);
    status_update.emit("Rewriting...");
    return true;
}

void TranscriptionRuntime::refuse_rewrite(const DictationJob& job) {
    // The user's rewrites run from other apps, so theirs also shows by the pointer. A program's
    // says nothing: its API answer is the report, and the user's own status stays.
    if (user_started(job.source)) {
        notify(report_busy("rewriting text"));
    } else {
        kLog.info("A program's action run was refused: {} is busy", identity().product);
    }
}

void TranscriptionRuntime::submit_rewrite(RewriteWork work) {
    {
        std::lock_guard lock(state_mutex_);
        rewrite_pending_ = false;
    }
    try {
        post_jobs([this, work = std::move(work)] { run_rewrite(work); });
    } catch (const std::exception& error) {
        kLog.error_with(error, "Could not start the rewrite");
        on_transcription_error(std::format("Could not start the rewrite: {}", error.what()));
    }
}

void TranscriptionRuntime::run_rewrite(const RewriteWork& work) {
    try {
        raise_if_canceled();
        RewriteResult result = work();
        raise_if_canceled();
        post_ui([this, result = std::move(result)] {
            on_transcription_complete(result.text, result.raw_text, result.info);
        });
    } catch (const CommandRefused& refused) {
        refuse(refused.what());
    } catch (const std::exception& error) {
        kLog.error_with(error, "Rewrite failed");
        report_failure(error.what());
    }
}

void TranscriptionRuntime::abandon_rewrite_job(std::string status) {
    {
        std::lock_guard lock(state_mutex_);
        if (!rewrite_pending_) {
            return;
        }
        rewrite_pending_ = false;
    }
    overlay_state_update.emit(appcore::OverlayState::None);
    if (slot_.cancel_requested()) {
        // A cancel during the selection read already said "Canceled"; it ends the job for
        // listeners as it would a running rewrite.
        const auto job = slot_.active_job();
        events_.publish(events::TRANSCRIPTION_FAILED,
                        events::transcription_failed(job.get(), events::FAILURE_CANCELED));
    } else if (!status.empty()) {
        status_update.emit(status);
        notify(status);
    }
    clear_pending_audio_metadata();
    finish_job();
}

void TranscriptionRuntime::refuse(const std::string& message) {
    // A refusal is not a failed transcription, so a Command Mode instruction's audio is not
    // kept: copying it into Recordings would push out older dictation audio, and its journal is
    // dropped so the next start does not recover it either. Dictated words are never dropped
    // this way: any other job keeps its audio like a failure.
    kLog.info("Nothing to paste for this command or transform");
    const auto job = slot_.active_job();
    if (job && job->mode == JobMode::Command) {
        drop_instruction_audio();
    }
    {
        std::lock_guard lock(state_mutex_);
        refused_ = true;
    }
    report_failure(message);
}

void TranscriptionRuntime::drop_instruction_audio() {
    std::optional<std::filesystem::path> pending;
    {
        std::lock_guard lock(state_mutex_);
        pending = pending_.audio_path;
    }
    if (pending && absolute_of(*pending) == absolute_of(recorder_.output_file())) {
        try {
            recorder_.acknowledge_recording();
        } catch (const std::exception& error) {
            kLog.error_with(error, "Could not drop the Command Mode recording");
        }
    }
    // The held slot keeps other jobs off both until on_transcription_error.
    std::lock_guard lock(state_mutex_);
    pending_.audio_path.reset();
}

void TranscriptionRuntime::deliver(const std::string& transcript,
                                   const std::shared_ptr<const DictationJob>& job,
                                   const std::string& notice,
                                   const std::optional<std::string>& before_cleanup) {
    if (job && job->mode == JobMode::Action) {
        // The words belong to the action, not to the focused app; it shows its own outcome
        // with the notice.
        ActionDelivery delivery;
        delivery.text = transcript;
        delivery.raw_text = before_cleanup.value_or(std::string());
        {
            std::lock_guard lock(state_mutex_);
            delivery.html = delivery_html_;
        }
        delivery.notice = notice;
        delivery.cancel = slot_.cancel_token();
        automation()->deliver(job, std::move(delivery));
        return;
    }
    if (job && job->mode == JobMode::Walkthrough) {
        deliver_walkthrough(transcript);
        return;
    }
    const std::string status_suffix = notice.empty() ? std::string() : " — " + notice;
    if (insert_into_scratchpad(transcript, status_suffix)) {
        return;
    }
    // A command or transform exists to replace text, so it pastes even with auto-paste off,
    // but only into the app it provably started in.
    const bool forced = job && job->mode != JobMode::Dictation;
    if (forced) {
        const PasteTarget target = paste_target(job.get(), services_.capture);
        if (target != PasteTarget::Same) {
            copy_rewrite(transcript, target == PasteTarget::Changed);
            return;
        }
    }
    const std::string text = text_for_paste(transcript, job.get());
    std::string html;
    {
        std::lock_guard lock(state_mutex_);
        html = delivery_html_;
    }
    const bool pasted = apply_clipboard_and_paste(
        text, status_suffix, html_for_paste(html, transcript, job.get()), forced);
    if (pasted) {
        after_paste(job.get(), text,
                    before_cleanup ? std::optional<std::string_view>(*before_cleanup)
                                   : std::nullopt,
                    preparer());
    }
    bool route_failed = false;
    {
        std::lock_guard lock(state_mutex_);
        route_failed = route_failed_;
    }
    if (job && job->route == "hotkey") {
        // Route by voice pasted instead of routing; say why, since the user asked for a route,
        // not a paste. A dictation that only could have been routed ("auto") stays as quiet as
        // any paste. A judge that could not answer is not the same as no action being named:
        // the words may have been meant for one.
        const std::string reason =
            route_failed ? "Couldn't reach Fast judgments" : "No action matched";
        notify(pasted ? reason + " — pasted instead" : reason);
    }
    if (forced) {
        notify_forced_outcome(*job, pasted);
    }
}

bool TranscriptionRuntime::paste_text_now(std::string text, const DictationJob* job) {
    const auto active = slot_.active_job();
    const bool busy =
        slot_.has_active_job() && (job == nullptr || !active || active->id != job->id);
    if (recorder_.is_recording() || busy) {
        notify(report_busy("pasting"));
        return false;
    }
    if (text.empty()) {
        return false;
    }
    const bool pasted = apply_clipboard_and_paste(text, {}, {}, true);
    if (!pasted) {
        notify(delivery_status_);
    }
    return pasted;
}

// --- History ----------------------------------------------------------------------------

Json TranscriptionRuntime::entry_stats_counts(const DictationJob* job) const {
    Json counts;
    {
        std::lock_guard lock(state_mutex_);
        counts = stats_counts_.is_object() ? stats_counts_ : Json::object();
        if (retranscription_) {
            counts["retranscription"] = true;
        }
    }
    if (job != nullptr && job->mode == JobMode::Action) {
        // Stats count an action's words under its name.
        try {
            if (const auto action =
                    automation()->find_action(settings_snapshot(), job->action_id)) {
                counts["target"] = action->name;
            }
        } catch (...) {
            kLog.debug("Action name unavailable for Stats");
        }
    }
    return counts;
}

Json TranscriptionRuntime::history_context(const DictationJob* job,
                                           const std::optional<CleanupInfo>& info,
                                           bool live) const {
    try {
        std::optional<std::string_view> level;
        if (info) {
            level = info->level;
        }
        std::optional<Json> settings;
        {
            std::lock_guard lock(state_mutex_);
            settings = profile_settings_;
        }
        return history_fields(job, level, live, settings ? *settings : settings_snapshot());
    } catch (...) {
        kLog.warning("History context unavailable; saving without it");
        return Json::object();
    }
}

std::string TranscriptionRuntime::model_info_for_history() const {
    const std::string engine = speech::engine(settings_snapshot());
    if (!is_local_engine(engine)) {
        return engine;
    }
    return std::format("{} ({})", engine, speech_.readiness(speech::Slot::Dictation).device_info);
}

void TranscriptionRuntime::queue_history(std::vector<QueuedEntry> entries) {
    // The slot stays held until the save ends, which also keeps the next recording from
    // replacing the WAV while it is copied.
    std::weak_ptr<int> alive = alive_;
    persistence_.post([this, alive, entries = std::move(entries)] {
        if (alive.lock()) {
            persist_history(entries);
        }
    });
}

void TranscriptionRuntime::acknowledge_if_recording(const std::filesystem::path& path) {
    if (absolute_of(path) == absolute_of(recorder_.output_file())) {
        recorder_.acknowledge_recording();
    }
}

void TranscriptionRuntime::persist_history(const std::vector<QueuedEntry>& entries) {
    std::string error;
    try {
        for (const QueuedEntry& queued : entries) {
            const AddEntry& fields = queued.fields;
            HistoryEntry entry;
            if (queued.retry_entry_id) {
                // A retry keeps the failed entry's recording, date, duration and source name.
                FailedRecordingRetry retry;
                retry.text = fields.text;
                retry.model = fields.model;
                retry.transcription_time = fields.transcription_time;
                retry.raw_text = fields.raw_text;
                retry.cleanup_provider = fields.cleanup_provider;
                retry.cleanup_model = fields.cleanup_model;
                retry.context = fields.columns;
                entry = history_.complete_failed_recording(*queued.retry_entry_id, retry);
            } else {
                auto saved = history_.add_entry(fields);
                if (!saved) {
                    throw std::runtime_error("the entry was not saved");
                }
                entry = std::move(*saved);
            }
            const auto kind = fields.columns.find("entry_kind");
            events_.publish(events::HISTORY_SAVED,
                            events::history_saved(queued.job_id, entry.id,
                                                  kind != fields.columns.end() && kind->second
                                                      ? *kind->second
                                                      : std::string()));
            // Stats are a side table: their failure never touches the entry or the status.
            record_stats(Json::object(), entry, [&](const Json&, const HistoryEntry& row) {
                stats_.record(dictation_stats::EntryFields::of(fields), &row);
            });
            const auto& source = fields.source_audio_path;
            std::error_code missing;
            if (source && std::filesystem::is_regular_file(*source, missing)) {
                if (!entry.audio_file || entry.audio_file->empty()) {
                    error = "Transcript saved, but audio could not be retained; recovery copy kept";
                } else {
                    acknowledge_if_recording(*source);
                }
            }
        }
    } catch (const std::exception& failure) {
        kLog.error_with(failure, "Failed to save transcription to history");
        error = std::format("History could not be saved: {}. Copy the transcript before closing.",
                            failure.what());
    }
    post_ui([this, error] { on_history_persisted(HistoryOutcome{error, true, {}}); });
}

void TranscriptionRuntime::save_failed_recording(const std::filesystem::path& path,
                                                 std::string status, const PendingAudio& pending,
                                                 bool remote_retry_exhausted) {
    FailedRecordingSave outcome = FailedRecordingSave::HistoryFailed;
    try {
        AddEntry fields;
        fields.text = "";
        fields.model = model_info_for_history();
        fields.source_audio_path = path;
        fields.audio_duration = pending.audio_duration;
        fields.file_size = pending.file_size;
        fields.source_name = pending.source_name;
        fields.columns["entry_kind"] = std::string(FAILED_RECORDING_KIND);
        const auto entry = history_.add_entry(fields);
        if (entry && entry->audio_file && !entry->audio_file->empty()) {
            acknowledge_if_recording(path);
            outcome = FailedRecordingSave::Saved;
        } else {
            outcome = FailedRecordingSave::AudioNotRetained;
        }
    } catch (const std::exception& error) {
        kLog.error_with(error, "Failed to save failed recording in History");
    }
    const StatusNotice report =
        failed_recording_status(std::move(status), outcome, remote_retry_exhausted);
    post_ui([this, report] {
        on_history_persisted(HistoryOutcome{report.status, true, report.notice});
    });
}

void TranscriptionRuntime::preserve_failed_audio(const std::filesystem::path& path,
                                                 std::string status) {
    try {
        const auto name = history_.preserve_recording(path);
        if (name) {
            status = preserved_audio_status(std::move(status), PreservedAudio::Saved, *name);
            acknowledge_if_recording(path);
        } else {
            std::error_code error;
            status = preserved_audio_status(std::move(status),
                                            std::filesystem::is_regular_file(path, error)
                                                ? PreservedAudio::NotRetained
                                                : PreservedAudio::Missing);
        }
    } catch (const std::exception& error) {
        kLog.error_with(error, "Failed to preserve audio after transcription error");
        status = preserved_audio_status(std::move(status), PreservedAudio::Failed);
    }
    post_ui([this, status] { on_history_persisted(HistoryOutcome{status, false, {}}); });
}

// --- cancel -----------------------------------------------------------------------------

bool TranscriptionRuntime::cancel() {
    const bool capturing = recorder_.is_recording();
    // A capture a Cancel already threw away is only closing its stream.
    const bool discarded = capturing && recorder_.capture_canceled();
    const bool recording = capturing && !discarded;
    kLog.info("Cancel called. Recording: {}", recording ? "True" : "False");
    // Under the slot's lock settle() reads it with: a job either sees this cancel before it
    // delivers, or was already settled.
    const CancelRequest request = slot_.request_cancel();
    if (request.repeat || (discarded && !request.working)) {
        return true; // the first Cancel is already stopping it
    }
    const bool started = recording || request.working;
    {
        std::lock_guard lock(state_mutex_);
        if (started) {
            cancel_started_ = clock_.steady();
        } else {
            cancel_started_.reset();
        }
    }
    if (started) {
        slot_.end_preview();
        events_.publish(events::RECORDING_CANCELED, events::recording_canceled(request.job.get()));
    }
    if (recording) {
        cancel_recording();
        return true;
    }
    if (is_transcribing()) {
        cancel_transcription();
        return true;
    }
    if (request.working) {
        overlay_state_update.emit(appcore::OverlayState::Canceling);
        status_update.emit("Canceled");
    }
    return request.working;
}

void TranscriptionRuntime::cancel_recording() {
    if (StreamingRuntime* streaming = streaming_.load()) {
        streaming->cancel_session();
    }
    incremental_->discard();
    // Listeners can't yet read the cancel from the recorder.
    canceling_recording_ = true;
    try {
        announce_recording(false);
    } catch (...) {
        canceling_recording_ = false;
        throw;
    }
    canceling_recording_ = false;
    recorder_.cancel_recording();
    services_.delivery.clipboard->discard_prefetch();
    end_walkthrough(false);
    forget_recording();
    overlay_state_update.emit(appcore::OverlayState::Canceling);
    status_update.emit("Recording canceled");
    kLog.info("Recording canceled");
    if (!has_active_job()) {
        std::optional<double> cancel_s;
        {
            std::lock_guard lock(state_mutex_);
            if (cancel_started_) {
                cancel_s = seconds_since(*cancel_started_);
                cancel_started_.reset();
            }
        }
        if (cancel_s) {
            metrics::record("cancel_s", *cancel_s);
        }
    }
}

void TranscriptionRuntime::cancel_transcription() {
    // The job's cancel token, set by request_cancel, stops the decode between windows and drops
    // its queued windows.
    overlay_state_update.emit(appcore::OverlayState::Canceling);
    status_update.emit(std::string(TRANSCRIPTION_CANCELED));
    kLog.info("Transcription canceled");
}

// --- other jobs -------------------------------------------------------------------------

void TranscriptionRuntime::retranscribe_audio(std::filesystem::path audio_path) {
    if (activity_.snapshot().meeting_active) {
        status_update.emit("Meeting Mode is active — end it before retranscribing");
        return;
    }
    std::error_code error;
    if (!std::filesystem::exists(audio_path, error)) {
        kLog.error("Audio file not found for re-transcription");
        overlay_state_update.emit(appcore::OverlayState::None);
        status_update.emit("Error: Audio file not found");
        return;
    }
    if (recorder_.is_recording()) {
        report_busy("re-transcribing audio");
        return;
    }
    if (!claim_job(nullptr, true)) {
        report_busy("re-transcribing audio");
        return;
    }
    forget_recording();
    kLog.info("Re-transcribing audio file");
    {
        std::lock_guard lock(state_mutex_);
        retranscription_ = true;
        pending_.audio_path.reset();
        pending_.source_name = base_utf8::path_to_utf8(audio_path.filename());
    }
    overlay_state_update.emit(appcore::OverlayState::Processing);
    status_update.emit("Processing...");
    // The failed-entry lookup reads the database, which never happens on the UI thread.
    post_jobs([this, audio_path = std::move(audio_path)] {
        try {
            const auto failed = history_.failed_recording_for_path(audio_path);
            const auto size = static_cast<std::int64_t>(std::filesystem::file_size(audio_path));
            {
                std::lock_guard lock(state_mutex_);
                pending_.file_size = size;
                pending_.audio_duration = failed ? failed->audio_duration : std::nullopt;
                if (failed) {
                    retry_entry_id_ = failed->id;
                    pending_.source_name = failed->source_name;
                    deliver_to_clipboard_ = false;
                }
            }
            if (failed) {
                run_transcription_job(audio_path);
            } else {
                submit_transcription_job(audio_path);
            }
        } catch (const std::exception& failure) {
            kLog.error_with(failure, "Failed to start re-transcription");
            report_failure(start_failure_message(failure));
        }
    });
}

// --- uploads ----------------------------------------------------------------------------

void TranscriptionRuntime::set_scratchpad(ScratchpadPort* scratchpad) {
    scratchpad_ = scratchpad;
}

void TranscriptionRuntime::set_cleaner(TranscriptCleaner* cleaner) {
    cleaner_ = cleaner;
}

void TranscriptionRuntime::set_cleanup(std::shared_ptr<llm::TranscriptCleanup> cleanup) {
    std::shared_ptr<TranscriptCleaner> made;
    if (cleanup) {
        made = std::make_shared<LlmTranscriptCleaner>(std::move(cleanup));
    }
    std::lock_guard lock(state_mutex_);
    llm_cleaner_ = std::move(made);
}

void TranscriptionRuntime::set_text_preparer(const TextPreparer* preparer) {
    preparer_ = preparer;
}

std::string TranscriptionRuntime::recording_profile() const {
    std::lock_guard lock(state_mutex_);
    return recording_profile_ ? recording_profile_->id : std::string();
}

void TranscriptionRuntime::set_model_consent_hook(std::function<void()> hook) {
    std::lock_guard lock(state_mutex_);
    model_consent_ = std::move(hook);
}

bool TranscriptionRuntime::upload_job_active() const {
    return upload_job_.load();
}

void TranscriptionRuntime::report_backend_progress(std::string message, bool transcribing) {
    if (transcribing) {
        overlay_state_update.emit(appcore::OverlayState::Transcribing);
    }
    status_update.emit(message);
}

void TranscriptionRuntime::announce_transcription(const std::filesystem::path& audio_path) {
    // A backend that splits this file (the cloud engine, over its upload limit) starts with the
    // large-file notice and reports the steps after it through report_backend_progress; every
    // other file goes straight to transcribing, whatever its size.
    std::optional<double> file_size_mb;
    if (services_.hooks.large_file_size_mb) {
        file_size_mb = services_.hooks.large_file_size_mb(audio_path);
    }
    if (!file_size_mb) {
        overlay_state_update.emit(appcore::OverlayState::Transcribing);
        status_update.emit("Transcribing...");
        return;
    }
    kLog.info("Large file ({:.2f} MB), backend splits it", *file_size_mb);
    large_file_detected.emit(*file_size_mb);
    status_update.emit(std::format("Splitting large file ({:.1f} MB)...", *file_size_mb));
}

std::string
TranscriptionRuntime::decode_path(const std::filesystem::path& audio_path,
                                  const std::optional<RecognitionContext>& recognition) {
    const std::string engine = speech::engine(settings_snapshot());
    // recognition_context.transcribe: only a backend that takes a context gets one. Python's
    // Parakeet takes the language per request; ParakeetBackend does not say so yet, so a local
    // engine counts as taking one.
    const auto context =
        context_for_backend(recognition, speech_.supports_recognition(speech::Slot::Dictation) ||
                                             is_local_engine(engine));
    RecognitionOptions options;
    std::vector<std::string> phrases;
    if (context) {
        options.language = context->language;
        phrases = context->phrases;
    }
    const DecodingScope decoding(decoding_);
    // The engine loaded in the slot decides how the file reaches it, not the saved choice: a
    // reload still pending (an engine switch) leaves the previous engine loaded until it ends.
    const bool takes_files = speech_.accepts_files(speech::Slot::Dictation);
    try {
        if (!takes_files) {
            if (speech::decodes_incrementally(engine)) {
                // The dictation's own recording reuses the windows decoded while it was
                // recorded; any other file drops the early decode and is decoded whole.
                return incremental_->transcribe(audio_path, options, slot_.cancel_token(), phrases)
                    .transcript.text;
            }
            return speech::decode_file(speech_, audio_path, options, speech::Lane::Final,
                                       slot_.cancel_token())
                .transcript.text;
        }
        // An engine that takes files gets the whole file, splits it itself when it must, and
        // reports its steps as it goes.
        speech::DecodeJob job;
        job.input = audio_path;
        job.options = options;
        job.lane = speech::Lane::Final;
        job.cancel = slot_.cancel_token();
        job.phrases = phrases;
        std::weak_ptr<int> alive = alive_;
        job.on_progress = [this, alive](const std::string& message, bool transcribing) {
            if (alive.lock()) {
                report_backend_progress(message, transcribing);
            }
        };
        return speech_.run(std::move(job)).transcript.text;
    } catch (const EngineError&) {
        // The engine was switched between the question and the decode: the backend's own
        // complaint ("uploads audio files only") is no sentence for the user.
        if (!slot_.cancel_requested() &&
            speech_.accepts_files(speech::Slot::Dictation) != takes_files) {
            throw EngineError(std::string(kEngineSwitchedMessage));
        }
        throw;
    }
}

bool TranscriptionRuntime::upload_audio_file(std::filesystem::path audio_path,
                                             std::optional<double> duration_s) {
    if (activity_.snapshot().meeting_active) {
        status_update.emit("Meeting Mode is active — end it before uploading audio");
        return false;
    }
    std::error_code missing;
    if (!std::filesystem::exists(audio_path, missing)) {
        kLog.error("Uploaded audio file not found");
        overlay_state_update.emit(appcore::OverlayState::None);
        status_update.emit("Error: Audio file not found");
        return false;
    }
    if (recorder_.is_recording()) {
        report_busy("uploading audio");
        return false;
    }
    // A refused claim leaves the running job's metadata alone.
    if (!claim_job(nullptr, true)) {
        report_busy("uploading audio");
        return false;
    }
    forget_recording();
    upload_job_ = true;
    kLog.info("Processing uploaded audio file");
    {
        std::lock_guard lock(state_mutex_);
        // Started from inside the window, so its result stays there; the Upload File tab has
        // its own Copy buttons.
        deliver_to_clipboard_ = false;
        pending_.audio_path.reset();
        pending_.source_name = base_utf8::path_to_utf8(audio_path.filename());
    }
    overlay_state_update.emit(appcore::OverlayState::Processing);
    status_update.emit("Processing uploaded file...");
    try {
        const auto size = static_cast<std::int64_t>(std::filesystem::file_size(audio_path));
        {
            std::lock_guard lock(state_mutex_);
            pending_.file_size = size;
            pending_.audio_duration = duration_s;
        }
        raise_if_canceled();
        require_backend_ready();
        post_jobs(
            [this, audio_path = std::move(audio_path)] { transcribe_audio_file(audio_path); });
        return true;
    } catch (const std::exception& error) {
        kLog.error_with(error, "Failed to process uploaded audio");
        on_transcription_error(start_failure_message(error));
        return false;
    }
}

bool TranscriptionRuntime::upload_audio_files(BatchUploadRequest request) {
    if (request.items.empty()) {
        status_update.emit("No audio files to transcribe");
        return false;
    }
    if (request.items.size() == 1) {
        const BatchItem& item = request.items.front();
        return upload_audio_file(base_utf8::path_from_utf8(item.audio_path), item.duration_seconds);
    }
    if (activity_.snapshot().meeting_active) {
        status_update.emit("Meeting Mode is active — end it before uploading audio");
        return false;
    }
    for (const BatchItem& item : request.items) {
        std::error_code missing;
        if (!std::filesystem::exists(base_utf8::path_from_utf8(item.audio_path), missing)) {
            kLog.error("Uploaded audio file not found");
            overlay_state_update.emit(appcore::OverlayState::None);
            status_update.emit(std::format("Error: Audio file not found: {}", item.source_name()));
            return false;
        }
    }
    if (recorder_.is_recording()) {
        report_busy("uploading audio");
        return false;
    }
    if (!claim_job(nullptr, true)) {
        report_busy("uploading audio");
        return false;
    }
    forget_recording();
    upload_job_ = true;
    kLog.info("Processing {} uploaded audio files", request.items.size());
    // A batch carries its metadata in its request and result; the one-shot pending fields stay
    // empty so nothing stale leaks into a later single-file job.
    clear_pending_audio_metadata();
    {
        std::lock_guard lock(state_mutex_);
        deliver_to_clipboard_ = false;
    }
    overlay_state_update.emit(appcore::OverlayState::Processing);
    status_update.emit(std::format("Processing {} uploaded files...", request.items.size()));
    try {
        require_backend_ready();
        post_jobs([this, request = std::move(request)] { transcribe_batch(request); });
        return true;
    } catch (const std::exception& error) {
        kLog.error_with(error, "Failed to process uploaded audio files");
        on_transcription_error(start_failure_message(error));
        return false;
    }
}

std::string TranscriptionRuntime::transcribe_path(const BatchItem& item) {
    const auto path = base_utf8::path_from_utf8(item.audio_path);
    overlay_state_update.emit(appcore::OverlayState::Processing);
    announce_transcription(path);
    return decode_path(path, std::nullopt);
}

void TranscriptionRuntime::transcribe_batch(BatchUploadRequest request) {
    BatchSteps steps;
    steps.cancel_requested = [this] {
        return slot_.cancel_requested();
    };
    steps.transcribe = [this](const BatchItem& item) {
        return transcribe_path(item);
    };
    steps.clean = [this](const std::string& raw, const std::optional<std::string>& context,
                         std::optional<double> timeout_s) {
        Completion done = maybe_cleanup_transcript(raw, context, timeout_s);
        CleanedText cleaned{std::move(done.text), std::move(done.raw_text), std::move(done.info),
                            std::nullopt};
        std::lock_guard lock(state_mutex_);
        cleaned.failure = last_cleanup_failure_;
        return cleaned;
    };
    steps.progress = [this](int position, int total, const std::string& name) {
        batch_progress.emit(position, total, name);
    };
    steps.item_finished = [this](int position, bool ok, const std::string& text) {
        batch_item_finished.emit(position, ok, text);
    };
    try {
        std::optional<BatchResult> result = BatchRunner(std::move(steps), clock_).run(request);
        if (!result) {
            report_failure(std::string(TRANSCRIPTION_CANCELED));
            return;
        }
        batch_completed.emit(*result);
        post_ui([this, result = std::move(*result)] { on_batch_complete(result); });
    } catch (const std::exception& error) {
        kLog.error_with(error, "Batch transcription failed");
        report_failure(error.what());
    }
}

void TranscriptionRuntime::on_batch_complete(BatchResult result) {
    // The files are transcribed; only saving them holds the slot. A Cancel that got here first,
    // while the files ran or after, stopped whatever was left: what was done is kept, and the
    // status says so.
    const bool canceled = slot_.settle();
    const BatchUploadRequest& request = result.request;
    std::vector<const BatchItemResult*> succeeded;
    std::vector<const BatchItemResult*> saveable;
    for (const BatchItemResult& item : result.items) {
        if (item.succeeded()) {
            succeeded.push_back(&item);
            if (!blank(item.text)) {
                saveable.push_back(&item);
            }
        }
    }
    const bool is_empty =
        request.combine() ? blank(result.combined_text.value_or(std::string())) : saveable.empty();
    if (canceled && is_empty) {
        on_transcription_error(std::string(TRANSCRIPTION_CANCELED));
        return;
    }
    if (!request.combine() && succeeded.empty()) {
        const std::string first_error = result.items.empty()
                                            ? std::string("no files")
                                            : result.items.front().error.value_or(std::string());
        on_transcription_error(
            std::format("All {} files failed: {}", result.items.size(), first_error));
        return;
    }
    auto [display, raw_display] = batch_display(result, is_empty);

    // The Upload tab treats None while it still runs as a failure, so the transcript lands
    // first.
    transcription_completed.emit(display, raw_display, std::nullopt);
    overlay_state_update.emit(appcore::OverlayState::None);
    const double cleanup_time = result.effective_cleanup_time_s();
    transcription_stats.emit(
        TranscriptionStats{result.effective_transcription_time_s(), result.total_duration_seconds(),
                           result.total_file_size(),
                           cleanup_time > 0 ? std::optional<double>(cleanup_time) : std::nullopt});

    if (is_empty) {
        kLog.info("Empty batch result; skipping history");
        events_.publish(events::TRANSCRIPTION_FAILED,
                        events::transcription_failed(nullptr, events::FAILURE_NO_SPEECH));
        status_update.emit(std::string(EMPTY_ASR_MESSAGE));
        finish_job();
        return;
    }
    std::vector<QueuedEntry> entries = batch_history_entries(result, saveable);
    publish_batch_completions(entries);
    status_update.emit(batch_status(result, saveable.size(), canceled));
    queue_history(std::move(entries));
}

std::vector<TranscriptionRuntime::QueuedEntry> TranscriptionRuntime::batch_history_entries(
    const BatchResult& result, const std::vector<const BatchItemResult*>& saveable) const {
    const std::string model = model_info_for_history();
    std::vector<QueuedEntry> entries;
    if (result.request.combine()) {
        std::vector<std::string> names;
        names.reserve(result.items.size());
        for (const BatchItemResult& item : result.items) {
            names.push_back(item.item.source_name());
        }
        QueuedEntry entry;
        entry.fields.text = result.combined_text.value_or(std::string());
        entry.fields.model = model;
        entry.fields.transcription_time = result.effective_transcription_time_s();
        entry.fields.audio_duration = result.total_duration_seconds();
        entry.fields.file_size = result.total_file_size();
        entry.fields.raw_text = result.combined_raw_text;
        entry.fields.cleanup_provider = result.combined_cleanup_provider;
        entry.fields.cleanup_model = result.combined_cleanup_model;
        entry.fields.source_name = batch_source_name(names);
        entries.push_back(std::move(entry));
        return entries;
    }
    for (const BatchItemResult* item : saveable) {
        QueuedEntry entry;
        entry.fields.text = item->text;
        entry.fields.model = model;
        entry.fields.transcription_time = item->elapsed_s;
        entry.fields.audio_duration = item->item.duration_seconds;
        entry.fields.file_size = item->file_size;
        entry.fields.raw_text = item->raw_text;
        entry.fields.cleanup_provider = item->cleanup_provider;
        entry.fields.cleanup_model = item->cleanup_model;
        entry.fields.source_name = item->item.source_name();
        entries.push_back(std::move(entry));
    }
    return entries;
}

void TranscriptionRuntime::publish_batch_completions(std::vector<QueuedEntry>& entries) {
    const Json context = history_context(nullptr, std::nullopt, false);
    for (QueuedEntry& entry : entries) {
        entry.fields.columns = to_context(context);
        // One completion per saved file, as a single upload publishes.
        std::optional<CleanupInfo> cleanup;
        if (entry.fields.cleanup_provider && !entry.fields.cleanup_provider->empty()) {
            cleanup = CleanupInfo{*entry.fields.cleanup_provider,
                                  entry.fields.cleanup_model.value_or(std::string()),
                                  0.0,
                                  {}};
        }
        events_.publish(events::TRANSCRIPTION_COMPLETED,
                        events::transcription_completed(nullptr, entry.fields.text,
                                                        entry.fields.raw_text, context, cleanup,
                                                        entry.fields.audio_duration));
    }
}

void TranscriptionRuntime::recover_recordings() {
    const std::string status =
        services_.captures_root.empty()
            ? recover_interrupted_recordings(recorder_.output_file(), history_.recordings_folder())
            : recover_interrupted_recordings(recorder_.output_file(), history_.recordings_folder(),
                                             services_.captures_root);
    if (!status.empty()) {
        status_update.emit(status);
    }
}

void TranscriptionRuntime::on_capture_error(std::string message) {
    if (recorder_.last_capture_error() != message || recorder_.capture_canceled()) {
        return; // queued notification from a canceled or replaced capture
    }
    slot_.end_preview();
    announce_recording(false);
    status_update.emit(message + " — preserving captured audio");
    if (StreamingRuntime* streaming = streaming_.load()) {
        streaming->cancel_session();
    }
    incremental_->discard();
    if (has_active_job()) {
        return; // finish_recording_job observes last_capture_error after the save
    }
    if (claim_job(slot_.recording_job())) {
        post_jobs([this] { finish_recording_job(); });
    }
}

void TranscriptionRuntime::on_model_changed(std::string backend_id) {
    if (activity_.snapshot().meeting_active) {
        status_update.emit("End the meeting before changing transcription models");
        return;
    }
    const auto values = model_values();
    if (std::find(values.begin(), values.end(), backend_id) == values.end()) {
        return;
    }
    const std::string previous = speech::engine(settings_snapshot());
    save_model_selection(settings_, backend_id);
    kLog.info("Switched to model: {}", backend_id);
    device_info_changed.emit(std::string(), std::nullopt);
    if ((is_local_engine(backend_id) || is_local_engine(previous)) &&
        services_.hooks.request_reload) {
        services_.hooks.request_reload();
    }
    // After the reload is asked for, so that a preview for the new engine waits for its load
    // instead of sharing the engine still loaded.
    if (StreamingRuntime* streaming = streaming_.load()) {
        streaming->reconfigure();
    }
}

} // namespace fv::dictation
