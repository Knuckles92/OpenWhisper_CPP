#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "appcore/engine_reload.h"
#include "audio/backend.h"
#include "base/executor.h"
#include "base/json.h"
#include "base/signal.h"
#include "base/subscription.h"
#include "dictation/recording_control.h"
#include "hotkeys/defaults.h"
#include "remote/control.h"
#include "remote/link.h"
#include "speech/service.h"

namespace fv {

class ActivityState;
class CancelRouter;
class Recorder;
class SettingsStore;
class ShutdownPlan;

namespace dictation {
class HotkeyRuntime;
class TranscriptionRuntime;
} // namespace dictation

// What the application controller drives. Every pointer but `recorder` and `hotkeys` is
// required and must outlive the controller; an empty function takes the default named beside it.
struct ApplicationServices {
    SettingsStore* settings = nullptr;
    ActivityState* activity = nullptr;
    speech::SpeechService* speech = nullptr;
    dictation::TranscriptionRuntime* transcription = nullptr;
    // The dictation recorder, whose saved microphone order change_audio_device applies.
    Recorder* recorder = nullptr;
    CancelRouter* cancel = nullptr;
    // ctx.ui(): every public method runs here.
    Executor* ui = nullptr;
    // The startup database step (OpenWhisper's one-worker "database-startup" executor).
    Executor* startup = nullptr;
    // Startup recovery of interrupted recordings (Python's shared two-worker executor).
    Executor* jobs = nullptr;
    // Reading the microphone list for a new microphone order.
    Executor* io = nullptr;

    // The dictation engine's config for a load, from a settings snapshot (resolve_parakeet).
    // Required.
    std::function<speech::BackendConfig(const Json& settings)> resolve_engine;
    // db.ensure_initialized: throws, with the error's text, when the database is unusable.
    // Startup strand. Empty: nothing to check.
    std::function<void()> ensure_database;
    // What waits for the database: recover_recordings by default. Jobs strand.
    std::function<void()> recover_recordings;
    // The saved microphone order (audio::load_priority). Io strand. Empty: unchanged.
    std::function<std::vector<audio::DeviceKey>()> load_priority;
    // A load put this config's runtime into the process: its component stays leased until exit
    // (models::acquire_component). Empty: nothing to lease.
    std::function<void(const speech::BackendConfig&)> runtime_loaded;
    // The engine choice changed: the runtime questions asked this session may be asked again
    // (ModelRuntime::forget_runtime_prompts). UI thread. Empty: nothing to forget.
    std::function<void()> forget_runtime_prompts;
    // The live preview's set-up for an engine that loads nothing at startup (the API engine);
    // a loaded engine's comes with its load (StreamingRuntime::setup). UI thread. Empty: none.
    std::function<void()> setup_streaming;
    // notify_main_ui_ready's background jobs after the startup database step is queued: the
    // updater's leftover prune and its launch check. UI thread. Empty: none.
    std::function<void()> start_background_jobs;
    // What waits for the startup database after recovery is queued: the backup coordinator's
    // schedule. UI thread. Empty: none.
    std::function<void()> start_database_dependents;
};

// The Remote engine's link while the dictation slot holds it: what OpenWhisper's controller asks
// of RemoteRuntime (services/runtime/remote.py) and of the selected RemoteSpeechBackend. The app
// reads them through RemoteEngineService::dictation_link and the saved pairing. Any thread; an
// empty function asks nothing.
struct RemoteEngineHooks {
    // Reconnect now rather than at the next scheduled retry (RemoteRuntime.retry_now).
    std::function<void()> retry_now;
    // A background reconnect is running (RemoteRuntime.restoring).
    std::function<bool()> restoring;
    // What this computer calls the paired host; "" when it is not paired.
    std::function<std::string()> host_name;
    // The dictation engine's link and the Model field's choices; nullopt without a Remote
    // engine (RemoteSpeechBackend.link and model_choices).
    std::function<std::optional<remote::RemoteLink>()> link;
    std::function<std::optional<remote::RemoteModels>()> model_choices;
    // Has the next load switch the paired computer to `model`, with runtime `changes` when
    // given (RemoteEngineService.request_model).
    std::function<void(const std::string& family, const std::string& model,
                       const std::optional<Json>& changes)>
        request_model;
    // Host: the paired computers connected to this one's engine now.
    std::function<std::vector<remote::ConnectedClient>()> connected_clients;
};

// The Qt-free half of OpenWhisper's ApplicationController (services/application_controller.py)
// for M1's Parakeet dictation: the refusal gates in front of the transcription runtime, the
// engine reload machine and its driver over the speech service, the deferred startup database
// step and what waits for it, the user's Cancel, the microphone order, and the M1 steps of Quit.
// It is the RecordingControl the hotkey runtime drives, so a shortcut passes the same gates as
// the window's buttons.
// Affinity: UI actor on services.ui; the RecordingControl queries are thread-safe (they read
// the transcription runtime's), initialize_startup_database runs on the startup strand.
class ApplicationController final : public dictation::RecordingControl {
public:
    explicit ApplicationController(ApplicationServices services);
    ~ApplicationController() override;
    ApplicationController(const ApplicationController&) = delete;
    ApplicationController& operator=(const ApplicationController&) = delete;

    // The hotkey runtime, which is built over this controller; set once both exist.
    void set_hotkey_runtime(dictation::HotkeyRuntime* hotkeys) { hotkeys_ = hotkeys; }
    // The Remote engine's link (RemoteRuntime); set once the remote service exists. UI thread.
    void set_remote_engine(RemoteEngineHooks hooks) { remote_ = std::move(hooks); }

    // ---- the Remote engine (services/runtime/remote.py RemoteRuntime) ----
    // remote_models: what the Model field shows.
    std::optional<remote::RemoteModels> remote_models() const;
    // The link with its next retry, for the engine card; nullopt without a Remote engine.
    std::optional<remote::RemoteLink> remote_link() const;
    // select_remote_model: switches the paired computer on the next reload ("Switching <host>
    // to <label>..."), or says why not and has the Model field shown again.
    void select_remote_model(const std::string& family, const std::string& model);
    // select_remote_runtime: the runtime fields' change, the same way ("Updating the runtime
    // on <host>...").
    void select_remote_runtime(const std::string& family, const std::string& model,
                               const Json& changes);
    // retry_now, from the offline link's click.
    void retry_remote();
    // The app's sources say the link reads differently, or which computers are connected
    // here changed (any thread): re-emitted on the UI thread below.
    void remote_link_poked();
    void remote_clients_poked();
    Signal<> remote_link_changed;
    // Show the Model field from remote_models() again (refresh_remote_models).
    Signal<> remote_models_stale;
    Signal<std::vector<remote::ConnectedClient>> remote_clients_changed;

    // ---- RecordingControl (for HotkeyRuntime) ----
    bool is_recording() const override;
    bool recorder_busy() const override;
    bool canceling_recording() const override;
    bool capture_canceled() const override;
    bool stream_open() const override;
    bool start(std::string source) override { return start_recording(std::move(source)); }
    void stop() override { stop_recording(); }
    bool toggle() override { return toggle_recording(); }
    bool cancel() override;
    void minimize_to_tray() override;
    void paste_last_original() override;
    Signal<bool>& recording_state_changed() override;

    // ---- recording ----
    // False when the start was refused (a meeting, backup, an engine not ready, or the
    // recorder): the caller rolls its recording chrome back.
    bool start_recording(std::string source = "hotkey");
    void stop_recording();
    // The hotkey's toggle: the gates are checked only when not recording, so a stop always
    // works. False when a start was refused.
    bool toggle_recording();
    // A recording formatted with the cleanup profile `profile_id`: refused during a meeting or a
    // backup like any start, but without the readiness check (the runtime answers an engine that
    // is not ready itself). False when refused.
    bool start_profile_recording(const std::string& profile_id, std::string source = "hotkey");
    // A shortcut starts its explicitly selected action or route; pressing while recording stops.
    bool toggle_action_recording(const std::string& action_id);
    bool toggle_route_recording();
    // A profile's shortcut: stops a recording in progress (whose profile stays the one it started
    // with), else starts one with the profile.
    void toggle_profile_recording(const std::string& profile_id);
    // History's Re-transcribe and Retry transcription.
    void retranscribe_audio(const std::filesystem::path& audio_path);
    // The engine picker chose `backend_id` ("parakeet", "api", "remote").
    void on_model_changed(const std::string& backend_id);
    // Settings saved a new microphone order: applied to the recorder unless it records.
    void change_audio_device();
    // A local engine's model, device or language changed (Quick Record's engine fields).
    void on_local_engine_settings_changed();
    // A Settings page saved something of `kind`: "transforms", "hotkeys" and "actions"
    // re-register the profile and transform shortcuts.
    void on_settings_changed(std::string_view kind);
    void update_hotkeys(const hotkeys::HotkeyList& hotkeys);
    void update_recording_trigger_mode(std::string_view mode);

    // ---- engine ----
    // reload_whisper_model: the debounced reload, refused during a meeting or a recording. A
    // non-empty note replaces "Reloading speech engine..." (the controller's _reload_note).
    void reload_whisper_model(std::string_view note = {});
    // Why capture must not start yet, starting the reload or model set-up that fixes it.
    std::optional<std::string> transcription_readiness_message();
    bool is_transcribing() const;
    bool is_meeting_active() const;
    // Meeting Mode borrows the engine (release_local_engine / restore_local_engine).
    bool release_local_engine();
    void restore_local_engine();
    // The two as the meeting's engine lease calls them, from its start or a post-meeting pass on
    // another thread (MeetingRuntime._model_lease): each runs on the UI executor, at once when
    // called there. The release waits up to `wait` for its answer (false when the controller is
    // gone or the UI thread did not answer in time); the restore does not wait. Any thread.
    std::function<bool()>
    lease_release_hook(std::chrono::milliseconds wait = std::chrono::seconds(10));
    std::function<void()> lease_restore_hook();
    appcore::EngineReloadMachine& reload_machine() { return reload_; }
    // The config of the engine the dictation slot has loaded (Ready or warming); nullopt while
    // none is. Any thread.
    std::optional<speech::BackendConfig> loaded_engine() const;
    // The first runtime folder a load put into this process; empty until one did. A runtime
    // stays loaded until exit (architecture 5.7). Any thread.
    std::filesystem::path loaded_runtime_dir() const;
    // A key was saved or removed: the API engine, when it is the one selected, asks for its key
    // again (OpenWhisper's backend resolves it at each availability check). Nothing changes
    // while a reload, a recording or a transcription runs.
    void refresh_api_engine();

    // ---- startup ----
    // Once the main window is shown: the first engine load (the saved engine is Parakeet), then
    // the startup database step, once.
    void notify_main_ui_ready();
    // The startup database step; on the startup strand.
    void initialize_startup_database();
    // Its answer on the UI thread: an error is shown and starts nothing; otherwise recovery runs.
    void on_startup_database_ready(const std::string& error);
    bool startup_database_ready() const { return startup_database_ready_; }
    const std::string& update_readiness_error() const { return update_readiness_error_; }

    // ---- quit ----
    // Adds the M1 steps of A03-29 to `plan` (the strands, the persistence drain and the
    // database close are added by whoever owns those).
    void add_shutdown_steps(ShutdownPlan& plan);
    bool shutting_down() const { return shutting_down_; }

    // The status line (status_update).
    Signal<std::string> status_update;
    // (readout, engine usable) for the engine card (device_info_update).
    Signal<std::string, std::optional<bool>> device_info_update;
    Signal<bool> engine_busy_changed;
    // The minimize shortcut (minimize_to_tray_requested): the window toggles on the UI thread.
    Signal<> minimize_to_tray_requested;
    // The Paste original shortcut, once its keys are up.
    Signal<> paste_last_original_requested;
    // (old, new) microphone names after a recording moved to another microphone.
    Signal<std::string, std::string> recording_device_switched;
    // The selected model is not installed: the set-up that adopts or picks one (the download
    // consent flow arrives with Downloads).
    Signal<> model_setup_requested;
    // A load left no engine and nothing says why: offer the model's runtime (the runtime consent
    // dialog arrives with Downloads).
    Signal<std::string> runtime_consent_requested;
    // The user's Cancel found nothing (NOTHING_TO_CANCEL): status and notice.
    Signal<std::string> nothing_to_cancel;

private:
    bool refuse_dictation();
    void drive_engine();
    void start_engine_load();
    void on_engine_readiness(speech::Slot slot, const speech::Readiness& readiness);
    // RemoteRuntime._on_settled for what the Remote engine's own link reports outside a load: a
    // host that dropped or came back updates the engine card, and the live preview is set up
    // again once it is back.
    void on_remote_link(const speech::Readiness& readiness);
    void on_engine_warmed(speech::Slot slot);
    // Selects the saved engine's config in the speech service without the reload machine: an
    // engine that loads no local model (the API engine) at startup and after a key change.
    void select_engine_now();
    void track_loaded(const speech::Readiness& readiness);
    Json settings_snapshot() const;

    ApplicationServices services_;
    SettingsStore& settings_;
    ActivityState& activity_;
    speech::SpeechService& speech_;
    dictation::TranscriptionRuntime& transcription_;
    CancelRouter& cancel_;
    Executor& ui_;
    dictation::HotkeyRuntime* hotkeys_ = nullptr;
    RemoteEngineHooks remote_;
    // The Remote engine's readout last shown, so a host that stays away is not announced again
    // (RemoteRuntime._reported). UI thread.
    std::string remote_reported_;

    appcore::EngineReloadMachine reload_;
    // The config the machine's start_load selected, until its readiness comes back.
    std::optional<speech::BackendConfig> loading_;
    // The config last sent to the speech service, whatever selected it. UI thread.
    std::optional<speech::BackendConfig> selected_;
    mutable std::mutex loaded_mutex_; // guards loaded_ and loaded_runtime_dir_
    std::optional<speech::BackendConfig> loaded_;
    std::filesystem::path loaded_runtime_dir_;
    // The speech service announced that load (its Loading); readiness before it is stale.
    bool load_announced_ = false;
    bool warming_ = false;
    // A lease arrived during a load: the engine that load brings up is released when it ends
    // (OpenWhisper's lease waits for the load under the engine lock, then frees the model).
    bool release_after_load_ = false;

    bool startup_database_requested_ = false;
    bool startup_database_ready_ = false;
    std::string update_readiness_error_;
    std::atomic<bool> shutting_down_{false};

    std::vector<Subscription> links_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace fv
