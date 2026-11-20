#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/events.h"
#include "base/executor.h"
#include "base/json.h"
#include "base/signal.h"
#include "base/subscription.h"
#include "meeting/build_options.h"
#include "meeting/cloud_pass.h"
#include "meeting/engine.h"
#include "meeting/engine_options.h"
#include "meeting/intelligence.h"
#include "meeting/model_lease.h"
#include "meeting/recovery.h"
#include "meeting/refinalize.h"
#include "meeting_state/repository.h"
#include "meeting_state/store.h"
#include "meeting_web/dashboard_api.h"

namespace fv {
class ActivityState;
class SettingsStore;
namespace net {
class Reactor;
}
namespace speech {
class SpeechService;
}
namespace audio {
class Backend;
}
namespace meeting_web {
class MeetingWebServer;
}
} // namespace fv

// services/runtime/meeting.py MeetingRuntime: Meeting Mode's lifecycle above the engine. It
// starts, pauses and ends meetings, bridges the engine's events to the desktop (status line,
// errors, the Meeting tab's state), keeps the finalization card (Keep for later, background
// finishing, retries), opens dashboards (live, past through an archive server, the report view,
// the guest link), runs the recovery scan and its actions, and manages Past Meetings (copy,
// delete, clear, retitle). The consent round-trip and the AI insights toggle are meeting
// intelligence's (set_consent_gate); remote meeting speech and the record sync are the paired
// computer's.
//
// Threads: every public method but the ones marked otherwise runs on the UI actor (ctx.ui()).
// Blocking work runs on named worker threads (`spawn`); engine events are bridged on the UI
// actor. The state below is guarded by one mutex that is never held while a signal is emitted,
// an engine is called or the repository is read.
namespace fv::meeting {

// "Seconds cleanup() waits for the engine's shutdown before leaving it to its own thread": the
// engine's own budget is minutes, and cleanup runs after the window and tray are gone.
inline constexpr std::chrono::milliseconds kShutdownJoinTimeout{5000};

// ---- pure helpers (golden vectors) ----

// redact_meeting_url: the dashboard origin with the capability token stripped,
// "<scheme>://<netloc>/<redacted>"; "" for an empty URL, "<redacted>" for one without a scheme or
// a host, or that urlsplit refuses.
std::string redact_meeting_url(std::string_view url);
// _with_history_target: `url` with history=<meeting_id> (and view=<view> when given) set in its
// query, the other parameters kept in their order, the whole query re-encoded as urlencode does.
std::string with_history_target(std::string_view url, std::string_view meeting_id,
                                std::string_view view = {});
// _ended_message: the status line when a meeting ends.
std::string ended_message(const Json& payload, std::string_view status, bool finalizing);
// _recovered_follow_up_step: the first post-meeting step a recovered meeting still needs
// ("redecode", "polish", "consolidation"), or nullopt.
std::optional<std::string> recovered_follow_up_step(const Json& meeting, const Json& settings);

// A refusal retitle_saved_meeting reports as it is ("record_busy: ...", "update_rejected: ...");
// the MCP renamer turns it into agentapi::ControlError.
class MeetingControlError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A running dashboard's title surface: the server with its REST API (MeetingWebServer and
// meeting_web::DashboardApi in the app).
// Affinity: thread-safe; the calls block (up to 15 s) and never run on the server's own threads.
class DashboardTitles {
public:
    virtual ~DashboardTitles() = default;
    virtual bool is_running() const = 0;
    // Renames through the dashboard's state so its open sockets see it; false when refused.
    virtual bool retitle_saved_meeting(const std::string& meeting_id, const std::string& title) = 0;
    // Adopts a rename another owner wrote.
    virtual void refresh_saved_meeting_title(const std::string& meeting_id) = 0;
};

// The titles of `server` through `api`; both must outlive the result.
std::shared_ptr<DashboardTitles> dashboard_titles(meeting_web::MeetingWebServer& server,
                                                  meeting_web::DashboardApi& api);

// One meeting's engine as the runtime drives it: MeetingEngine in the app, fakes in tests.
// Affinity: thread-safe; start, shutdown and wait_for_end block.
class RuntimeEngine {
public:
    virtual ~RuntimeEngine() = default;
    // "" before the meeting row exists.
    virtual std::string meeting_id() const = 0;
    virtual bool is_active() const = 0;
    virtual Subscription add_listener(Executor& executor,
                                      std::function<void(const MeetingEvent&)> fn) = 0;
    // {meeting_id, url, host_url, guest_url}; throws a sentence for the user.
    virtual Json start() = 0;
    // Undoes a start whose result is unusable (_abort_start).
    virtual void abort_start() = 0;
    // Whether End or Quit already claimed the meeting (_processing_stop).
    virtual bool processing_stopped() const = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void end() = 0;
    virtual std::optional<meeting_agent::MeetingAgentRoute> live_intelligence_destination() const {
        return std::nullopt;
    }
    virtual void set_live_intelligence(const std::optional<LiveIntelligenceApproval>&,
                                       const CancelToken& = {}) {}
    // True once the End worker finished (or there is none).
    virtual bool wait_for_end(std::chrono::milliseconds timeout) = 0;
    virtual void shutdown() = 0;
    virtual bool publish_finalization(const Json& finalization) = 0;
    virtual std::shared_ptr<meeting_state::MeetingStateStore> store() const = 0;
    // The pause-aware meeting clock, in seconds.
    virtual double meeting_time_s() const = 0;
    virtual void allow_agent_writes() = 0;
    virtual void revoke_agent_writes() = 0;
    // The dashboard serving the meeting, or null before its server started.
    virtual std::shared_ptr<DashboardTitles> dashboard() const = 0;
};

// A past meeting's dashboard after a restart (ArchivedMeetingDashboard on its own server).
// Affinity: thread-safe.
class RuntimeArchive {
public:
    virtual ~RuntimeArchive() = default;
    virtual std::string meeting_id() const = 0;
    virtual bool is_running() const = 0;
    virtual std::string host_url() const = 0;
    virtual std::shared_ptr<meeting_state::MeetingStateStore> store() const { return {}; }
    virtual void shutdown() = 0;
    virtual std::shared_ptr<DashboardTitles> dashboard() const = 0;
};

// _require_dashboard_address: a start result without "host_url" or "url" aborts the start and
// throws std::runtime_error(messages::kDashboardNoAddress).
void require_dashboard_address(RuntimeEngine& engine, const Json& result);

using EngineFactory = std::function<std::shared_ptr<RuntimeEngine>(MeetingEngineOptions options)>;
// The archive of `meeting` (a repository row) on a started server bound to `bind` and `port`.
using ArchiveFactory = std::function<std::shared_ptr<RuntimeArchive>(
    const Json& meeting, const std::string& bind, std::uint16_t port)>;
// Starts `task` on a thread of its own named `name` (Python's daemon worker threads).
using WorkerSpawner = std::function<void(std::string name, Task task)>;
// meeting.refinalize.rerun_finalization (tests replace it).
using RerunFunction = std::function<Json(const std::string& meeting_id, const RerunRequest&)>;
// meeting.recovery.find_recoverable_meetings over the runtime's repository.
using RecoveryScan = std::function<std::vector<Json>(meeting_state::MeetingRepository&)>;
// meeting.recovery.finalize_meeting for (meeting row, language, lease).
using RecoveredFinalizer =
    std::function<bool(meeting_state::MeetingRepository&, const Json& meeting,
                       const std::string& asr_language, ModelLease& lease)>;

struct MeetingRuntimeDeps {
    // The UI actor's executor (ctx.ui()): every public method runs there, every signal's
    // subscribers too when they connect to it.
    Executor* ui = nullptr;
    SettingsStore* settings = nullptr;
    std::shared_ptr<meeting_state::MeetingRepository> repository;
    // The History routes' page source (SqlMeetingRepository's adapter); null: empty pages.
    std::shared_ptr<meeting_web::PastMeetingPages> past_meetings;
    // config.MEETINGS_FOLDER.
    std::filesystem::path meetings_root;
    // nullptr: system_clock().
    const Clock* clock = nullptr;
    // The public meeting.* events; nullptr: null_event_sink().
    EventSink* events = nullptr;
    // Backup, host update, recording and transcription gates; nullptr reads as idle.
    ActivityState* activity = nullptr;
    // null: null_intelligence().
    std::shared_ptr<IntelligenceProvider> intelligence;
    std::function<std::shared_ptr<llm::Judge>()> local_fast_judge;
    // The insight review's judge: device-local, with its own per-question budget of at least
    // `at_least_s` (meeting_intel::InsightReviewDeps::judge). Unset: local_fast_judge as it is.
    std::function<std::shared_ptr<llm::Judge>(double at_least_s)> local_review_judge;
    std::function<Json(std::shared_ptr<meeting_state::MeetingStateStore>,
                       const meeting_agent::MeetingAgentRoute&, std::string_view, Executor&,
                       const CancelToken&, std::function<bool()>)>
        start_saved_report;
    // Called on the desktop UI for the exact saved meeting and resolved destination.
    std::function<bool(const Json&, const meeting_agent::MeetingAgentRoute&, bool report)>
        confirm_saved_insights;
    std::function<DiarizeFn()> approved_speaker_decoder;
    // The dictation engine's lease (release_local_engine / restore_local_engine), also handed to
    // the engine's and the archive's re-runs and to recovery; null: NullModelLease.
    std::shared_ptr<ModelLease> model_lease;
    // webbrowser.open; null: nothing opens (logged).
    std::function<bool(std::string_view url)> open_url;
    // controller.local_whisper_loading_message: why the local engine cannot take a meeting yet,
    // or nullopt.
    std::function<std::optional<std::string>()> engine_loading_message;
    // controller.ensure_local_model_available, before a retry that may re-decode.
    std::function<void()> ensure_local_model_available;
    // Forgets the event bus's replay buffer after a meeting's rows are deleted; empty:
    // automation::clear_bus.
    std::function<void()> forget_replayable_events;
    // What a start reads besides the settings (spool_root defaults to meetings_root).
    BuildOptionsSources sources;

    // The app's engine and archive need these (unused when both factories are given).
    speech::SpeechService* speech = nullptr;
    // Every live start, hotkeys included, asks again on the UI thread for this named computer.
    std::function<bool(const Json&)> confirm_live_audio;
    std::function<bool(const std::string&, const meeting_agent::MeetingAgentRoute&)>
        confirm_browser_insights;
    std::function<std::unique_ptr<speech::Backend>(std::shared_ptr<Json>)> live_remote_backend;
    std::shared_ptr<audio::Backend> audio;
    net::Reactor* reactor = nullptr;
    // The "meeting" strand, where a live meeting's heartbeat runs.
    Executor* strand = nullptr;

    // Seams; an empty one takes the app's default.
    EngineFactory engine_factory;
    ArchiveFactory archive_factory;
    WorkerSpawner spawn;
    RerunFunction rerun;
    RecoveryScan find_recoverable;
    RecoveredFinalizer finalize_recovered;
    // The engine a redecode or a headless finalize decodes with; empty builds MeetingAsrEngine
    // on `speech`.
    AsrFactory asr;
    // The diarizer a re-run's redecode labels system audio with; empty: speaker_model_diarizer
    // over sources.speaker_model_path and sources.onnxruntime_path.
    DiarizerFactory diarizer;
};

class MeetingRuntimeProbe;

// Affinity: UI actor (see the namespace comment); retitle_saved_meeting, meeting_busy,
// meeting_in_use, is_claimed, is_finalizing and copy_past_meeting_transcript run on any thread.
class MeetingRuntime {
public:
    explicit MeetingRuntime(MeetingRuntimeDeps deps);
    // Waits for every worker thread of its own.
    ~MeetingRuntime();
    MeetingRuntime(const MeetingRuntime&) = delete;
    MeetingRuntime& operator=(const MeetingRuntime&) = delete;

    // At database ready: the recovery scan and the restore of the last finalization card, each
    // on a worker.
    void setup();
    void start_meeting(std::optional<bool> cloud = std::nullopt,
                       std::string system_audio_policy = "auto", std::string intent = {});
    void start_demo_meeting(std::optional<bool> cloud = std::nullopt,
                            std::string system_audio_policy = "auto");
    // Ends a running meeting; otherwise asks for the platform acknowledgement unless a meeting is
    // claimed (meeting_platform_ack_requested), whose answer starts one.
    void toggle_from_hotkey();
    std::optional<std::pair<std::string, meeting_agent::MeetingAgentRoute>>
    live_intelligence_destination() const;
    void set_live_intelligence(const std::string& meeting_id,
                               std::optional<LiveIntelligenceApproval> approval);
    // Set by the consent gate for the currently claimed start.
    void set_start_intelligence_approval(LiveIntelligenceApproval approval);
    void pause();
    void resume();
    void end();
    void open_dashboard();
    void open_report();
    void copy_guest_link();
    void retry_insights();
    void retry_speakers();
    bool can_offer_speaker_upload() const;
    void request_speaker_upload(std::string meeting_id,
                                std::function<bool(const SpeakerUploadOffer&)> confirm);
    // From a step id, or "failed" for the earliest failed one; `meeting_id` defaults to the
    // engine's meeting, then the card's, then the newest meeting.
    void retry_step(std::string step_id, std::optional<std::string> meeting_id = std::nullopt);
    // Moves the ended meeting to the background once its local speech model is free; nothing
    // while finalization is not running here, the model is held, or another meeting is claimed.
    void continue_in_background();
    // Keep for later: hides the card and persists the deferral so it stays hidden across
    // restarts. Refused (with a status line) while finalization runs or a meeting is claimed.
    void defer_finalization_card();
    void show_past_meeting(std::string meeting_id);
    void open_past_meeting(std::string meeting_id, bool report = false);
    // The exported transcript, or "" (with a status line or an error); blocks on the repository.
    std::string copy_past_meeting_transcript(std::string meeting_id);
    void delete_past_meeting(std::string meeting_id, bool delete_recordings = true);
    void clear_past_meetings(bool delete_recordings = false);
    void finalize_recovered(std::string meeting_id);
    void discard_recovered(std::string meeting_id);
    void retry_recovery_scan();
    // Whether this computer may still write to the meeting: live or finishing, in the
    // background, re-running its post-meeting steps.
    bool meeting_busy(const std::string& meeting_id) const;
    // meeting_busy, or a dashboard here may be showing it.
    bool meeting_in_use(const std::string& meeting_id) const;
    // An MCP or API title edit, through the running dashboard that owns the meeting (the others
    // refreshed), else the repository. Throws MeetingControlError (messages::kRecordBusy,
    // kUpdateRejected) and the repository's py::ValueError for an invalid title.
    void retitle_saved_meeting(const std::string& meeting_id, const std::string& title);
    // Meeting intelligence's consent round-trip (M8). Called with the cloud choice and a
    // continuation; returns true when it took the start over and will call the continuation
    // with whether AI insights were granted, false when the start may go on as it is.
    void set_consent_gate(std::function<bool(bool cloud, std::function<void(bool)> answer)> gate);
    // App exit: the archive server, the engine's shutdown bounded by kShutdownJoinTimeout (a
    // stuck one is logged and left to its own thread) and the background meetings within the
    // same budget.
    void cleanup();

    // A session is running (paused included).
    bool is_active() const;
    // During start (the consent round-trip included) as well as an active meeting; post-meeting
    // finalization is not a claim, so dictation unlocks once capture ends.
    bool is_claimed() const;
    bool is_finalizing() const;

    // Python's signals.
    Signal<Json> meeting_state_changed;
    Signal<std::string> meeting_status_update;
    Signal<std::string> meeting_error;
    Signal<Json> meeting_server_started;
    Signal<std::vector<Json>> meeting_recovery_found;
    Signal<std::string> meeting_recovery_scan_failed;
    Signal<> meeting_platform_ack_requested;
    Signal<std::string> meeting_guest_link_ready;
    Signal<> past_meetings_refresh_requested;
    // (meeting id, kind, payload) of every event of the current engine, after the runtime
    // handled it.
    Signal<std::string, std::string, Json> meeting_engine_event;
    // A meeting start stops History playback first (the argument says why: "meeting").
    Signal<std::string> playback_stop_requested;
    // The main window's status line (controller.status_update).
    Signal<std::string> status_update;

private:
    friend class MeetingRuntimeProbe;

    struct Retry;
    // One engine event on its way to the UI actor, with the retry generation and claim that
    // were current when it was queued.
    struct QueuedEvent {
        std::shared_ptr<RuntimeEngine> engine;
        std::string kind;
        Json payload;
        std::uint64_t generation = 0;
        std::shared_ptr<FinalizationClaim> claim;
    };

    // ---- shared (runtime.cpp) ----
    Json settings_snapshot() const;
    void spawn(std::string name, Task task);
    void post_ui(Task task);
    void sync_activity();
    bool engine_active(const std::shared_ptr<RuntimeEngine>& engine) const;
    bool is_active_locked() const;
    bool is_claimed_locked() const;
    double monotonic_s() const;
    Json clock_fields(const std::shared_ptr<RuntimeEngine>& engine) const;
    void refresh_past_meetings();
    void release_local_engine();
    void restore_local_engine();
    std::shared_ptr<ModelLease> model_lease() const;

    // ---- start (detail/runtime_start.cpp) ----
    void begin_start(std::optional<bool> cloud, bool demo, std::string system_audio_policy,
                     std::string intent);
    void on_consent_result(bool demo, bool granted);
    void launch(bool cloud, bool demo, std::string system_audio_policy);
    void start_worker(bool cloud, bool demo, std::string system_audio_policy,
                      std::optional<Json> approved_route);
    void start_failed(const std::shared_ptr<RuntimeEngine>& engine, const std::string& error);
    std::optional<std::vector<std::string>>
    adopt_started_engine(const std::shared_ptr<RuntimeEngine>& engine, const Json& result);
    void announce_started(const std::shared_ptr<RuntimeEngine>& engine, const Json& result,
                          bool cloud, bool demo);
    MeetingEngineOptions make_options(bool cloud, bool demo, const std::string& system_audio_policy,
                                      const CancelToken& cancel = {}) const;
    // Cancels what the engine's own threads may still be blocked in (its speaker model
    // download) before it is shut down; once per engine.
    void cancel_engine_work(const RuntimeEngine* engine);
    // pause, resume and end run the engine call on a worker. The engine's events are held while
    // one is in flight and routed after its outcome, so the desktop sees them in Python's order
    // (the call's state first, then what the engine reported meanwhile).
    void hold_engine_events(const std::shared_ptr<RuntimeEngine>& engine);
    void release_engine_events(const RuntimeEngine* engine);

    // ---- the card (detail/runtime_card.cpp) ----
    bool hydrate_finalization_card(const Json& meeting, bool reveal);
    Json meeting_identity_fields(const Json& meeting, const Json* finalization) const;
    bool persist_card_deferred(const std::string& meeting_id, bool deferred);
    void hide_finalization_card();
    void finish_background_meeting(const std::string& meeting_id,
                                   std::shared_ptr<RuntimeEngine> engine);
    // Persists the leftover card's deferral (on a worker) and hides it, then runs `then` on the
    // UI actor; at once when there is no finished card to file.
    void file_leftover_card(Task then);
    Json meeting_content_summary(const std::string& meeting_id,
                                 const std::optional<Json>& meeting = std::nullopt);
    Json speaker_rerun_fields() const;

    // ---- retry (detail/runtime_retry.cpp) ----
    bool retry_refused(const std::string& step_key);
    struct SavedAgentPermit {
        meeting_agent::MeetingAgentRoute route;
        std::function<bool()> allowed;
    };
    std::optional<SavedAgentPermit> approve_saved_agent(const Json& meeting, bool report,
                                                        const CancelToken& cancel);
    bool retry_needs_agent(const Json& meeting, const std::string& step);
    void prepare_retry(std::string step_key, std::string meeting_id);
    void claim_and_run_retry(std::string step_key, std::string meeting_id,
                             std::optional<SavedAgentPermit> permit = {});
    std::shared_ptr<Retry> claim_retry(const std::string& meeting_id, const std::string& step_key);
    void retry_worker(const std::shared_ptr<Retry>& retry);
    Json rerun_finalization(const std::shared_ptr<Retry>& retry);
    void finish_retry(const std::shared_ptr<Retry>& retry, Json finalization,
                      const std::string& message);
    Json stored_finalization(const std::string& meeting_id);
    void publish_finalization(const std::string& meeting_id, const Json& finalization,
                              const std::shared_ptr<RuntimeEngine>& engine,
                              std::optional<std::uint64_t> retry_generation);
    RerunRequest rerun_request(const Json& meeting, const std::string& from_step,
                               ModelLease* lease) const;
    meeting_web::PostMeetingPasses post_meeting_passes(std::shared_ptr<ModelLease> lease);
    void request_browser_insights(std::string meeting_id, CancelToken permit,
                                  std::function<void(std::optional<std::string>)> finish);

    // ---- dashboards and past meetings (detail/runtime_past.cpp) ----
    void open_past_meeting_worker(const std::string& meeting_id, const std::string& view);
    void show_past_meeting_worker(const std::string& meeting_id);
    void report_dashboard_error(const std::string& message);
    bool open_url(const std::string& url);
    void delete_past_meeting_worker(const std::string& meeting_id, bool delete_recordings);
    void clear_past_meetings_worker(bool delete_recordings);
    void forget_deleted(const std::string& meeting_id, bool has_origin);
    void shutdown_archive_dashboard();

    // ---- recovery (detail/runtime_recovery.cpp) ----
    void recovery_scan_worker();
    void restore_last_finalization_worker();
    void finalize_recovered_worker(const std::string& meeting_id);
    void discard_recovered_worker(const std::string& meeting_id);

    // ---- the engine event bridge (detail/runtime_bridge.cpp) ----
    void listen(const std::shared_ptr<RuntimeEngine>& engine);
    void queue_engine_event(const std::shared_ptr<RuntimeEngine>& engine, std::string kind,
                            Json payload, std::optional<std::uint64_t> retry_generation = {});
    void route_engine_event(const QueuedEvent& event);
    void on_engine_event(const std::shared_ptr<RuntimeEngine>& engine, const std::string& kind,
                         const Json& payload);
    void on_status_event(const std::shared_ptr<RuntimeEngine>& engine, const Json& payload);
    void on_server_started_event(const Json& payload);
    void on_error_event(const Json& payload);
    void on_background_ready_event();
    void on_ended_event(const Json& payload);
    void on_intelligence_event(const Json& payload);
    // Normalizes `finalization` into the card, adds it to `state_payload`, and says what the
    // status line shows.
    void apply_finalization(const Json& finalization, Json& state_payload,
                            std::optional<std::string> meeting_id);
    void shutdown_engine(std::optional<std::chrono::milliseconds> timeout);

    // ---- the app's engine and archive (detail/runtime_engine.cpp) ----
    EngineFactory default_engine_factory();
    ArchiveFactory default_archive_factory();
    AsrFactory default_asr_factory();

    MeetingRuntimeDeps deps_;
    Executor& ui_;
    const Clock& clock_;
    EventSink& events_;
    std::shared_ptr<IntelligenceProvider> intelligence_;
    std::shared_ptr<ModelLease> model_lease_;
    // Cancelled by cleanup(): background waits and re-runs stop early.
    CancelSource shutdown_;

    mutable std::mutex mutex_;
    std::shared_ptr<RuntimeEngine> engine_;
    std::map<const RuntimeEngine*, Subscription> listeners_;
    std::optional<std::string> host_url_;
    std::optional<std::string> guest_url_;
    std::shared_ptr<RuntimeArchive> archive_;
    bool archive_starting_ = false;
    // Claimed by a start for the whole attempt, the consent round-trip included.
    bool starting_ = false;
    // Foreground finalization blocks another meeting until it moves to the background; it does
    // not claim dictation.
    bool finalizing_ = false;
    // controller.meeting_active: Meeting Mode's exclusive flag, set once a start announced.
    bool controller_meeting_active_ = false;
    std::uint64_t retry_generation_ = 0;
    std::optional<std::string> retry_meeting_id_;
    std::shared_ptr<FinalizationClaim> retry_claim_;
    bool background_ready_ = false;
    std::map<std::string, std::shared_ptr<RuntimeEngine>, std::less<>> background_engines_;
    std::map<std::string, std::string, std::less<>> background_host_urls_;
    std::set<std::string, std::less<>> background_workers_;
    std::condition_variable background_done_;
    std::optional<Json> finalization_;
    std::optional<std::string> card_meeting_id_;
    std::vector<std::string> deferred_start_errors_;
    std::string pending_system_audio_policy_ = "auto";
    std::string pending_intent_;
    std::optional<LiveIntelligenceApproval> pending_agent_approval_;
    std::function<bool(bool, std::function<void(bool)>)> consent_gate_;
    // Whether the dictation engine is released to a meeting start (flag-guarded restore).
    bool engine_released_ = false;
    // Per engine (live, background or starting): cancels its speaker model resolver. Children
    // of shutdown_, so cleanup() and the destructor cancel them all.
    std::map<const RuntimeEngine*, CancelSource> engine_cancels_;
    // Engine events held while a pause, resume or end call is in flight (hold_engine_events).
    struct HeldEvents {
        int calls = 0;
        std::vector<QueuedEvent> events;
    };
    std::map<const RuntimeEngine*, HeldEvents> held_events_;
    // Tests: runs first in every finalization publication (it may throw or wait).
    std::function<void(const std::string& meeting_id, const Json& finalization)> publish_hook_;
    // Tasks posted to the UI actor run through it; the destructor closes it.
    std::shared_ptr<CallbackGuard> alive_ = std::make_shared<CallbackGuard>();

    // Worker threads of the default spawner.
    struct Thread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex threads_mutex_;
    std::vector<Thread> threads_;
};

} // namespace fv::meeting
