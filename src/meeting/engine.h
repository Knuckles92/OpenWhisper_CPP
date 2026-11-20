#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "audio/backend.h"
#include "base/cancel.h"
#include "base/clock.h"
#include "base/executor.h"
#include "base/json.h"
#include "base/subscription.h"
#include "diarize/clustering.h"
#include "meeting/end_run.h"
#include "meeting/engine_options.h"
#include "meeting/intelligence.h"
#include "meeting/model_lease.h"
#include "meeting/store_adapters.h"
#include "meeting_capture/asr_engine.h"
#include "meeting_capture/capture_session.h"
#include "meeting_capture/failure_log.h"
#include "meeting_capture/meeting_clock.h"
#include "meeting_state/ids.h"
#include "meeting_state/repository.h"
#include "meeting_state/store.h"
#include "meeting_state/types.h"
#include "meeting_web/dashboard_engine.h"
#include "meeting_web/server.h"
#include "net/reactor.h"
#include "speech/service.h"

// meeting/engine.py MeetingEngine: one meeting's whole pipeline. It owns the capture (sources,
// spools, the watchdog), the meeting's ASR engine and diarizer, the state store and the dashboard
// server, and it runs End: drain, the terminal status, `ended`, the offline re-decode and the
// finalization steps. With AI insights on it also runs the live meeting agent: a core from the
// IntelligenceProvider behind a CheckpointScheduler, health reports, one approved backup agent,
// the cloud toggle and note requests; and the local fast features (voice commands, live
// highlights, the optional insight review). End shuts the live agent down with capture and,
// when it was online and healthy, runs cleanup and the report each through a fresh core of the
// provider; otherwise the card reads kIntelligenceOfflineMessage. A build whose provider is
// unavailable has no AI panel and keeps cloud_enabled off.
//
// Lifecycle: construct, start(), pause()/resume() while live, end() (returns at once; the work
// runs on "fv-meeting-end"), shutdown() at exit. The dashboard keeps serving the final state
// after End until shutdown().
namespace fv::llm {
class Judge;
}
namespace fv::meeting_intel {
class CitationVerifier;
class LiveHighlights;
class VoiceCommandListener;
} // namespace fv::meeting_intel

namespace fv::meeting {
namespace detail {
class LiveMeetingAgent;
}

// Seconds between repository heartbeats while a meeting is live.
inline constexpr double HEARTBEAT_INTERVAL_S = 10.0;
// The ASR drain budget of a normal End, and of an End at app shutdown.
inline constexpr double END_DRAIN_TIMEOUT_S = 300.0;
inline constexpr double SHUTDOWN_DRAIN_TIMEOUT_S = 30.0;
// The worst wait for the post-End consolidation pass; only shutdown's join budget uses it.
inline constexpr double CONSOLIDATION_TIMEOUT_S = 900.0;
// How long End waits for an in-flight start() before unwinding a partly built pipeline anyway.
inline constexpr double START_WAIT_TIMEOUT_S = 120.0;
// Guest display names are clipped to this many code points.
inline constexpr std::size_t MAX_GUEST_NAME_LEN = 80;

// Why speaker separation is unavailable (the banner and Getting started); a failed or blocked
// model download supplies its own sentence.
inline constexpr std::string_view DIARIZATION_OFF = "Speaker identification is off in Settings.";
inline constexpr std::string_view DIARIZATION_NO_MODEL = "No speaker model is installed.";
inline constexpr std::string_view DIARIZATION_LOAD_FAILED =
    "The speaker model could not load on this computer.";
inline constexpr std::string_view DIARIZATION_DEGRADED =
    "Speaker separation stopped working during the meeting.";
inline constexpr std::string_view DIARIZATION_DEMO = "The demo meeting uses channel labels.";

inline constexpr std::string_view kIntelligenceOfflineMessage =
    "Meeting intelligence is offline; final cloud insights could not run.";

// One listener event: status, segments, server_started, error, ended, asr_released,
// background_ready (meeting intelligence adds its own kinds).
struct MeetingEvent {
    std::string kind;
    Json payload;
};

// The meeting's live ASR as the engine drives it: MeetingAsrEngine in the app (adapt_asr_engine),
// a fake in the engine's tests.
// Affinity: as MeetingAsrEngine (thread-safe; callbacks on its own threads).
class MeetingAsr {
public:
    virtual ~MeetingAsr() = default;
    virtual bool load_backend(const CancelToken& cancel) = 0;
    virtual bool is_available() const = 0;
    virtual std::string last_error() const = 0;
    virtual void start(ChunkResult on_chunk_result) = 0;
    virtual bool enqueue(const meeting_state::SpooledChunk& chunk) = 0;
    virtual std::size_t requeue_pending() = 0;
    virtual bool drain(std::chrono::milliseconds timeout) = 0;
    virtual void stop() = 0;
    virtual void start_preview(PreviewCallback callback) = 0;
    // Called on capture device threads: never blocks.
    virtual void feed_preview(const CaptureBlock& block, double start_s) noexcept = 0;
    virtual void stop_preview() = 0;
    virtual std::vector<meeting_state::TranscriptSegment>
    transcribe_offline_session(const std::filesystem::path& spool_dir,
                               const std::vector<Json>& chunks, OfflineProgress progress,
                               const CancelToken& cancel) = 0;
};

// MeetingAsrEngine as the engine's ASR. With `lease_from`, it holds that service's EngineLease
// ("meeting") until it stops, so no engine reload swaps the model under the meeting.
std::unique_ptr<MeetingAsr> adapt_asr_engine(std::unique_ptr<MeetingAsrEngine> engine,
                                             speech::SpeechService* lease_from = nullptr);

// The meeting's ASR for (meeting id, model, language "auto" or a code, callbacks); nothing
// loads until load_backend.
using MeetingAsrFactory = std::function<std::unique_ptr<MeetingAsr>(
    const std::string& meeting_id, const std::string& model, const std::string& language,
    AsrCallbacks callbacks)>;

// The dashboard server as the engine drives it: meeting_web::MeetingWebServer in the app.
// Affinity: thread-safe.
class DashboardTransport {
public:
    virtual ~DashboardTransport() = default;
    // The base URL, or why the server could not start.
    virtual std::expected<std::string, std::string> start() = 0;
    virtual void stop() = 0;
    virtual void broadcast(const Json& message, bool host_only) = 0;
    // A transport without host-only delivery drops host-only messages rather than leaking them
    // to guests.
    virtual bool supports_host_only() const { return true; }
    virtual std::string base_url() const = 0;
    virtual std::string host_url() const = 0;
    virtual std::string guest_url() const = 0;
    // Closes every socket after the links rotated.
    virtual void invalidate_connections() = 0;
};

class MeetingEngine;

// The dashboard for an engine, bound to "localhost" or "lan" on `port` (0: any).
using DashboardFactory = std::function<std::unique_ptr<DashboardTransport>(
    MeetingEngine& engine, const std::string& bind, std::uint16_t port)>;

// The live diarizer over the speaker model and the ONNX Runtime library (either may be missing),
// whose new speakers `host` creates; null when there is none. The host outlives it.
using EngineDiarizerFactory = std::function<std::unique_ptr<diarize::Diarizer>(
    const std::optional<std::filesystem::path>& model,
    const std::optional<std::filesystem::path>& onnxruntime, diarize::DiarizerHost& host,
    const std::string& meeting_id)>;

// The id of this process (os.getpid()), written with every heartbeat.
std::int64_t current_process_id();

struct EngineDeps {
    std::shared_ptr<meeting_state::MeetingRepository> repository;
    // The speech service the default ASR factory decodes on.
    speech::SpeechService* speech = nullptr;
    // Dedicated to the current live session. Released after its ASR workers have stopped.
    std::shared_ptr<speech::SpeechService> live_remote_speech;
    // Capture devices; may be null for a demo meeting.
    std::shared_ptr<audio::Backend> audio;
    // The reactor the default dashboard serves on.
    net::Reactor* reactor = nullptr;
    // nullptr: system_clock(). Meeting time, timestamps and the speaker-pass date.
    const Clock* clock = nullptr;
    // nullptr: meeting_state::random_ids().
    meeting_state::IdSource* ids = nullptr;
    // null: null_intelligence().
    std::shared_ptr<IntelligenceProvider> intelligence;
    // Empty: diarize::create_diarizer over the options' speaker model and ONNX Runtime.
    EngineDiarizerFactory diarizer;
    // The dashboard's REST routes (meeting_web's API), installed by the default dashboard.
    meeting_web::MeetingWebServer::RouteInstaller install_routes;
    // The settings the voice command guide reads at every hello.
    std::function<Json()> settings;
    std::function<std::shared_ptr<llm::Judge>()> local_fast_judge;
    // The insight review's judge (RuntimeDeps::local_review_judge); unset: local_fast_judge.
    std::function<std::shared_ptr<llm::Judge>(double at_least_s)> local_review_judge;
    // The lease dashboard re-runs take around a re-decode model; null: none.
    std::shared_ptr<ModelLease> model_lease;
    // Whether an OpenAI key is configured, for the speaker pass's gate; empty reads as none.
    std::function<bool()> openai_key_present;
    // nullopt: this process.
    std::optional<std::int64_t> pid;
    // The app's "meeting" strand, where the heartbeat runs; null: a thread of the engine's own.
    Executor* strand = nullptr;

    // Seams; an empty one takes the default.
    MeetingAsrFactory asr;
    DashboardFactory dashboard;
    std::optional<SourceFactory> capture_sources;
    std::optional<CaptureTiming> capture_timing;
    // The (host, guest) capability tokens of a new meeting and of a rotation.
    std::function<std::pair<std::string, std::string>()> tokens;
    // Tests only: start() runs the processing start on its own thread instead of
    // "fv-meeting-start".
    bool processing_inline = false;
};

class MeetingEngineProbe;

// Affinity: thread-safe. start() and shutdown() block (call them off the GUI thread); end()
// returns at once. Listeners run inline when their executor is current on the emitting thread,
// else they are posted; what a listener throws is logged and swallowed. No lock of the engine is
// held while a listener runs, a status is broadcast or a capture, ASR or diarizer call is made,
// except the lifecycle lock across an ASR stop and the adoption of a component (whose callbacks
// never take it).
class MeetingEngine final : public meeting_web::DashboardEngine {
public:
    MeetingEngine(MeetingEngineOptions engine_options, EngineDeps engine_deps);
    // Shuts down, then waits for every thread of the engine however long it takes. Never on
    // one of those threads (on_engine_thread): release_off_engine_threads.
    ~MeetingEngine() override;
    MeetingEngine(const MeetingEngine&) = delete;
    MeetingEngine& operator=(const MeetingEngine&) = delete;

    // Whether the calling thread is one of the engine's own (End, the processing start, the
    // diarizer start, the backup switch), which the destructor joins.
    bool on_engine_thread() const;

    Subscription add_listener(Executor& executor, std::function<void(const MeetingEvent&)> fn);

    // Creates the meeting and brings the pipeline up: {meeting_id, url, host_url, guest_url}.
    // The dashboard and one live capture source are required; the ASR model, the speaker model
    // and meeting intelligence start afterwards and degrade on their own. A failed start is torn
    // down (an empty meeting row is discarded) and rethrown: EngineError "Meeting engine is
    // already active or finished", "Meeting dashboard could not start: <reason>", or the capture's
    // error.
    Json start();
    // Freezes or unfreezes the meeting clock and the status; nothing once End claimed the
    // meeting. EngineError "Could not persist paused meeting state" / "...resumed..." when the
    // status cannot be saved (the clock is rolled back).
    void pause() override;
    void resume() override;
    // Finishes the meeting on "fv-meeting-end" and returns at once; a second End reuses it.
    void end() override;
    // Waits for the End worker; true when it finished (or there is none, or it is the caller).
    bool wait_for_end(std::chrono::milliseconds timeout);
    // Waits for the processing start, the speaker model's included; true when it finished.
    bool wait_for_processing(std::chrono::milliseconds timeout);
    // App exit: ends a live meeting with the short drain, waits for End (bounded), marks an
    // interrupted finalization, stops the dashboard and joins the start threads (5 s each).
    void shutdown();
    // Persists and shows a finalization snapshot a retry computed elsewhere; true when saved.
    bool publish_finalization(const Json& finalization);
    ModelLease& model_lease();

    // meeting_web::DashboardEngine
    std::string meeting_id() const override;
    std::shared_ptr<meeting_state::MeetingStateStore> store() const override;
    bool is_active() const override;
    Json recent_agent_activity() const override;
    void set_cloud_enabled(bool enabled) override;
    void request_note_adjustment(std::string text, std::function<void(Json)> completion) override;
    std::optional<meeting_agent::MeetingAgentRoute> live_intelligence_destination() const;
    void enable_live_intelligence(const meeting_agent::MeetingAgentRoute& route,
                                  std::optional<meeting_agent::MeetingAgentRoute> backup = {},
                                  const CancelToken& permit = {});
    std::vector<meeting_state::OpResult>
    apply_client_action(std::string_view actor, const std::optional<std::string>& participant_id,
                        const Json& op) override;
    std::vector<meeting_state::OpResult>
    undo(std::int64_t seq, const std::optional<std::string>& participant_id) override;
    // The new guest participant (name stripped, clipped to MAX_GUEST_NAME_LEN, "Guest" when
    // empty). EngineError "No active meeting"; py::ValueError with the store's reason (or
    // "guest_rejected").
    Json add_guest(std::string_view name) override;
    // {host_url, guest_url}; EngineError "No active meeting" before start.
    Json regenerate_tokens() override;
    std::filesystem::path spool_root() const override;

    const MeetingEngineOptions& options() const { return options_; }
    MeetingClock& clock() { return meeting_clock_; }

    std::vector<Json> get_transcript(double after_start_s = -1.0,
                                     std::optional<std::int64_t> limit = std::nullopt) const;
    std::vector<Json> get_transcript_delivery_page(std::int64_t after_seq = 0,
                                                   std::int64_t limit = 300) const;
    std::int64_t get_transcript_delivery_cursor(std::string_view consumer) const;
    void advance_transcript_delivery_cursor(std::string_view consumer, std::int64_t seq);
    std::int64_t get_transcript_delivery_high_water() const;

    // Agent tool writes are allowed for live checkpoints and the final consolidation only.
    void allow_agent_writes() { agent_writes_allowed_ = true; }
    void revoke_agent_writes() { agent_writes_allowed_ = false; }
    bool agent_writes_allowed() const { return agent_writes_allowed_; }

private:
    friend class MeetingEngineProbe;

    struct Listener {
        Executor* executor;
        std::function<void(const MeetingEvent&)> fn;
        std::shared_ptr<CallbackGuard> guard;
    };
    // A thread of the engine with the event it signals when it ends.
    struct Worker {
        std::thread thread;
        // Atomic: on_engine_thread reads it without the lifecycle lock, which a caller on an
        // engine thread may hold.
        std::atomic<std::thread::id> id;
        bool started = false;
        bool done = true;
    };
    // Set by the tests through MeetingEngineProbe before start().
    struct TestHooks {
        // End: before the capture stops (Python's monkeypatched capture.stop()).
        std::function<void()> before_drain;
        // End: right before the capture's flush.
        std::function<void()> before_flush;
        // End: where a complete meeting's final highlights run (meeting intelligence).
        std::function<void()> final_highlights;
        // start() leaves the processing start out (the test starts the ASR itself).
        bool skip_processing = false;
    };

    // ---- events (engine.cpp) ----
    void emit(std::string kind, Json payload);
    void broadcast(const Json& message, bool host_only = false);
    void emit_status(const std::string& note = {});
    void emit_ephemeral_finalization(const meeting_state::FinalizationState& finalization);
    bool set_finalization(const FinalizationUpdate& update);
    void adopt_untitled_title_from_topic();
    void apply_diarizer_pins(const std::vector<meeting_state::OpResult>& results);
    std::shared_ptr<DashboardTransport> server() const;
    std::shared_ptr<MeetingAsr> asr() const;
    std::shared_ptr<diarize::Diarizer> diarizer() const;
    std::string utc_now() const;

    // ---- start (detail/engine_start.cpp) ----
    void create_meeting_row(const std::string& id, const std::string& host_token,
                            const std::string& guest_token, const std::filesystem::path& spool_dir);
    void open_store(const std::string& id);
    std::string seed_demo_meeting();
    std::string start_server();
    void start_processing();
    void start_live_intelligence(std::optional<meeting_agent::MeetingAgentRoute> takeover = {},
                                 const CancelToken& permit = {}, std::string previous_failure = {});
    void schedule_backup_after_checkpoint_failure(const CancelToken& current, std::string failure);
    CancelSource final_insights_cancel() const;
    void start_fast_features();
    // _refresh_live_highlights_status: whether a live meeting's status changed.
    bool refresh_live_highlights_status();
    void observe_voice_commands(const std::vector<Json>&);
    void observe_voice_preview(const Json&);
    // _on_voice_feedback; with `persist`, engine._voice_feedback: the message is also stored with
    // the voice_feedback op ({message, at}).
    void emit_voice_feedback(const Json& feedback, bool persist = false);
    void request_spoken_recap(const std::string& spoken);
    void stop_fast_features();
    void observe_highlights(const std::vector<Json>&, double frontier);
    void finalize_highlights();
    void review_final_insights();
    // Whether the live agent is online as End begins (engine.py: the scheduler End reuses
    // exists), and whether its core can take a pass. Read before the drain shuts it down.
    struct LiveIntelligenceAtEnd {
        bool online = false;
        bool healthy = false;
    };
    LiveIntelligenceAtEnd live_intelligence_at_end() const;
    // The topic-shift judge the attached live agent's scheduler holds (meeting_agent::TopicJudge);
    // empty without one. For MeetingEngineProbe.
    using TopicJudgeFn = std::function<std::optional<double>(const std::string& previous,
                                                             const std::string& current)>;
    TopicJudgeFn attached_topic_judge() const;
    void stop_live_intelligence();
    void notify_intelligence_segments(int count);
    void notify_intelligence_guidance(const std::vector<meeting_state::OpResult>&);
    // One change of the live agent's state as it is published (engine.py
    // _on_intelligence_health, _report_intelligence_unusable, _unwind_intelligence_start).
    struct IntelligenceReport {
        bool online = false;
        // The `intelligence` event's error.
        std::string error;
        // The finalization card's sentence while the meeting is live; empty: `error`.
        std::string card;
        // The status event's note.
        std::string note;
        // What End's offline message names; nullopt: `error`.
        std::optional<std::string> failure;
    };
    void report_live_intelligence(IntelligenceReport report, const CancelToken& token = {});
    void processing_worker();
    void abort_start();
    bool aborted_meeting_has_artifacts(const std::string& id);
    bool discard_empty_meeting(const std::string& id);
    void mark_aborted_failed(const std::string& id);
    void start_heartbeat();
    void schedule_heartbeat();
    void heartbeat_tick();
    void stop_heartbeat();
    void set_worker_done(Worker& worker);
    bool wait_worker(const Worker& worker, std::chrono::milliseconds timeout);

    // ---- ASR (detail/engine_asr.cpp) ----
    void start_asr(bool defer_load);
    bool load_deferred_asr(const std::shared_ptr<MeetingAsr>& asr);
    void report_unavailable_asr(const std::shared_ptr<MeetingAsr>& asr);
    void adopt_asr(const std::shared_ptr<MeetingAsr>& asr);
    // Stops and forgets the ASR under the held lifecycle lock; false when stop() threw. Whether
    // asr_released is due comes back in `release_due`.
    bool stop_asr_locked(std::string_view context, bool& release_due);
    bool stop_asr(std::string_view context);
    void emit_asr_released();
    void on_chunk(const meeting_state::SpooledChunk& chunk);
    void on_chunk_result(const meeting_state::SpooledChunk& chunk,
                         std::vector<meeting_state::TranscriptSegment> segments);
    void on_asr_backlog_status(const std::string& message, bool overloaded);
    void on_asr_chunk_failure(const std::string& message);
    void on_speech_preview(const Json& preview);
    void feed_preview(const CaptureBlock& block) noexcept;

    // ---- speakers (detail/engine_diarize.cpp) ----
    void assign_speakers(std::vector<meeting_state::TranscriptSegment>& segments);
    std::vector<std::pair<std::optional<std::int64_t>, std::vector<std::size_t>>>
    label_mic_segments(std::vector<meeting_state::TranscriptSegment>& segments, bool have_diarizer);
    void start_diarizer_background();
    void diarizer_init_worker();
    void start_diarizer();
    std::unique_ptr<diarize::Diarizer> create_diarizer();
    void disable_diarizer();
    void resolve_diarization_model();
    bool prepare_diarizer(diarize::Diarizer* diarizer);
    void check_diarizer_degraded();
    void on_diarizer_relabel(const diarize::RelabelOps& ops);

    // ---- End (detail/engine_end.cpp) ----
    void await_start(std::chrono::milliseconds timeout);
    void begin_end(double drain_timeout_s);
    void end_worker(double drain_timeout_s);
    bool drain_end_capture(double drain_timeout_s);
    void mark_ending();
    std::int64_t unfinished_chunks_after_drain();
    bool will_redecode_at_end() const;
    bool will_identify_speakers_at_end() const;
    void repair_live_draft();
    void persist_terminal_status(const std::string& status);
    std::string intelligence_offline_message() const;
    Json finalization_summary_stats();
    bool run_offline_final_pass(const ProgressCallback& progress);
    Json run_cloud_speaker_pass(const ProgressCallback& progress,
                                const std::function<void()>& on_start, bool injected_decoder);
    void wrap_up_end(EndRun& run);
    void finish_failed_end(const std::string& error, const std::optional<std::string>& terminal,
                           bool ended_emitted);
    void record_failed_end_state(const std::string& error,
                                 const std::optional<std::string>& terminal,
                                 const std::string& status);
    void interrupt_finalization_on_shutdown();

    MeetingEngineOptions options_;
    EngineDeps deps_;
    const Clock& clock_;
    meeting_state::IdSource& ids_;
    std::shared_ptr<IntelligenceProvider> intelligence_;
    std::shared_ptr<ModelLease> model_lease_;
    FailureLog failures_;
    MeetingClock meeting_clock_;

    // The lifecycle lock: start, pause, resume, End's claim, the adoption of the ASR and the
    // diarizer, and an ASR stop. Never held while a listener runs.
    mutable std::mutex lifecycle_mutex_;
    std::atomic<bool> active_{false};
    bool starting_ = false;            // guarded by lifecycle_mutex_
    std::thread::id start_thread_;     // guarded by lifecycle_mutex_
    bool start_complete_ = true;       // guarded by lifecycle_mutex_
    bool end_claimed_ = false;         // guarded by lifecycle_mutex_
    bool asr_release_emitted_ = false; // guarded by lifecycle_mutex_
    std::atomic<bool> processing_stop_{false};
    CancelSource processing_cancel_;
    CancelSource final_highlights_cancel_;

    std::condition_variable lifecycle_changed_;
    Worker processing_; // guarded by lifecycle_mutex_
    Worker diarizer_init_;
    Worker backup_switch_;
    Worker end_;
    TestHooks hooks_;

    // The meeting's identity and components, read from every thread.
    mutable std::mutex state_mutex_;
    // Serializes user toggles; the provider and scheduler run outside state_mutex_.
    std::mutex intelligence_toggle_mutex_;
    CancelSource live_intelligence_cancel_{processing_cancel_.token()};
    CancelSource final_insights_cancel_;
    std::optional<meeting_agent::MeetingAgentRoute> live_approved_route_;
    bool starting_live_intelligence_ = false;
    std::optional<meeting_agent::MeetingAgentRoute> approved_backup_;
    bool tried_backup_ = false;
    std::string meeting_id_;
    std::filesystem::path spool_dir_;
    std::optional<std::string> me_participant_id_;
    std::shared_ptr<meeting_state::MeetingStateStore> store_;
    std::shared_ptr<DashboardTransport> server_;
    std::shared_ptr<MeetingAsr> asr_;
    std::shared_ptr<detail::LiveMeetingAgent> live_intelligence_;
    std::shared_ptr<meeting_intel::VoiceCommandListener> voice_commands_;
    std::atomic<std::int64_t> voice_feedback_seq_{0};
    std::shared_ptr<meeting_intel::CitationVerifier> citation_verifier_;
    std::shared_ptr<meeting_intel::LiveHighlights> live_highlights_;
    bool asr_ready_ = false;
    std::shared_ptr<diarize::Diarizer> diarizer_;
    std::optional<std::filesystem::path> diarization_model_path_;
    std::string offline_failure_detail_;
    std::string intelligence_failure_;
    // How many human-guidance notices reached a live agent (Python's FakeScheduler counts them).
    std::atomic<int> guidance_notices_{0};
    std::deque<Json> agent_activity_;
    // engine.py _agent_activity_last: when each (kind, tool) was last published (steady clock).
    std::map<std::pair<std::string, std::string>, std::chrono::steady_clock::time_point>
        agent_activity_last_;

    std::atomic<bool> degraded_diarization_{false};
    std::atomic<bool> agent_writes_allowed_{true};

    // Ordered replay: the ASR adoption holds it while it replays the pending SQLite chunks, so a
    // spool callback cannot overtake that replay.
    std::mutex chunk_mutex_;
    // The chunk index, the enqueued ids and the preview frontiers.
    mutable std::mutex index_mutex_;
    std::map<std::int64_t, meeting_state::SpooledChunk> chunk_index_;
    std::set<std::int64_t> enqueued_chunk_ids_;
    std::map<std::string, double, std::less<>> preview_frontiers_;

    // Shared with the subscriptions, which may outlive the engine.
    struct ListenerList {
        std::mutex mutex;
        std::vector<std::shared_ptr<Listener>> entries;
    };
    std::shared_ptr<ListenerList> listeners_;

    std::mutex heartbeat_mutex_;
    bool heartbeat_stopped_ = true; // guarded by heartbeat_mutex_
    TimerHandle heartbeat_timer_;   // guarded by heartbeat_mutex_
    std::shared_ptr<CallbackGuard> heartbeat_guard_ = std::make_shared<CallbackGuard>();
    std::unique_ptr<ThreadPool> own_pool_;

    // Every ASR the meeting built, kept until the engine is destroyed: the capture's device
    // threads may still be feeding a preview that raced its stop.
    std::mutex retired_mutex_;
    std::vector<std::shared_ptr<MeetingAsr>> retired_asr_;
    std::atomic<MeetingAsr*> preview_target_{nullptr};

    StoreDiarizerHost diarizer_host_;
    StoreCaptureSink capture_sink_;
    // Last: destroyed first, while everything it calls back into still exists.
    std::unique_ptr<MeetingCapture> capture_;
};

// Runs `release` (the owner's shutdown and destruction of `engine`) on this thread, or, on one
// of the engine's own threads, on a thread of its own that ~MeetingEngine can join this one
// from: a listener running inline on End that holds the engine's last reference ends up here.
void release_off_engine_threads(const MeetingEngine& engine, std::function<void()> release);

} // namespace fv::meeting
