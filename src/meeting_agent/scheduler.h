#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "agents/loopback_mcp.h"
#include "base/cancel.h"
#include "base/clock.h"
#include "base/json.h"
#include "meeting_agent/interfaces.h"
#include "meeting_state/store.h"

// meeting/agent/scheduler.py: when the meeting agent runs, and with which transcript. Rolling
// card checkpoints fire only when new transcript exists, on an adaptive interval (15 s base,
// 5 s under segment pressure, 20 s when quiet), after two minutes of meeting context, early on a
// topic shift between the last two 60-second windows. Note-taker and transcript-polish passes
// follow the card checkpoints on their own cadence, human guidance and note requests jump the
// queue, and the end of the meeting runs the final polish and the consolidation pass. Every
// pass runs on one worker thread ("fv-meeting-checkpoint"), so work that becomes due while a
// pass is in flight coalesces into the next one.
namespace fv::meeting_agent {

using Json = fv::Json;

// The engine side of the scheduler: transcript, meeting clock and the durable delivery ledger.
// The meeting engine implements it (FakeSchedulerHost in tests).
// Affinity: called from the scheduler's worker and from the thread that ends the meeting.
class SchedulerHost {
public:
    virtual ~SchedulerHost() = default;

    // The meeting's state store; null when there is none (a pass then cannot run).
    virtual std::shared_ptr<meeting_state::MeetingStateStore> store() = 0;
    // Transcript rows whose start_s is strictly after `after_start_s` (-1 for all of them), in
    // timeline order, with the meeting's term corrections applied. May throw.
    virtual std::vector<Json> get_transcript(double after_start_s) = 0;
    // Seconds of meeting so far, paused time excluded.
    virtual double meeting_clock_s() const = 0;
    // Up to `limit` transcript rows committed after delivery sequence `after_seq`, in commit
    // order, each with its "delivery_seq". May throw.
    virtual std::vector<Json> delivery_page(std::int64_t after_seq, int limit) = 0;
    // The acknowledged sequence of a consumer ("cards" or "notes"). May throw.
    virtual std::int64_t delivery_cursor(std::string_view consumer) = 0;
    // Acknowledges a consumer up to `seq`; never moves a cursor back. May throw.
    virtual void advance_delivery_cursor(std::string_view consumer, std::int64_t seq) = 0;
    // The newest delivery sequence of the meeting. May throw.
    virtual std::int64_t delivery_high_water() = 0;
    // Closes the engine's agent-write gate before a terminal consolidation.
    virtual void revoke_agent_writes() = 0;

    // Whether meeting_clock_s answers. Without a clock the warm-up is measured from the
    // scheduler's start and topic shifts are never looked for (Python's engine without `clock`).
    virtual bool has_meeting_clock() const { return true; }
    // Whether the delivery_* members answer. Without a ledger the scheduler re-reads a
    // time window behind the newest segment it sent (Python's engine without the delivery
    // methods).
    virtual bool has_delivery_ledger() const { return true; }
};

// The terminal outcome of an end-of-meeting pass: status "completed", "unavailable" or "failed"
// (never running or pending), and the sentence the finalization card keeps.
struct ConsolidationOutcome {
    std::string status;
    std::string message;

    // {"status", "message"}.
    Json to_json() const;
    bool operator==(const ConsolidationOutcome&) const = default;
};

// (previous_window_text, window_text) -> probability that the topic changed; nullopt is "no
// answer", and the content-word rule decides. May throw (read as no answer).
using TopicJudge =
    std::function<std::optional<double>(const std::string& previous, const std::string& current)>;

// meeting/finalization.py's polish_transcript, which the engine supplies (it lives beside the
// engine): cleans `segments` block by block through `agent`, each block with a fresh snapshot of
// `store`, `timeout_s` per block. nullopt (or "") when every block succeeded, else why the pass
// stopped.
// progress(detail, current_block, total_blocks).
using PolishTranscript = std::function<std::optional<std::string>(
    AgentCore& agent, meeting_state::MeetingStateStore& store, const std::vector<Json>& segments,
    double timeout_s, const std::function<void(const std::string&, int, int)>& progress)>;

// An installed agent (Claude Code, Codex, OpenCode) takes 10-20 s per run and draws on the
// user's own plan, so cards refresh about once a minute, notes every other checkpoint, and polish
// every few minutes.
inline constexpr CheckpointCadence INSTALLED_AGENT_CADENCE{45.0, 30.0, 90.0, 4, 180.0, 2, 120.0};

// The default card intervals (seconds) when neither the caller nor the core sets one.
inline constexpr double BASE_INTERVAL_S = 15.0;
inline constexpr double MIN_INTERVAL_S = 5.0;
inline constexpr double MAX_INTERVAL_S = 20.0;
// Meeting seconds of context before any automatic pass.
inline constexpr double INITIAL_CONTEXT_S = 120.0;
// Jaccard similarity of the two windows' content words below which the topic shifted.
inline constexpr double SHIFT_JACCARD_THRESHOLD = 0.15;
// Semantic judge probability at or above which the topic shifted. On 97 human-labelled AMI
// minute pairs this gave precision 0.69 at recall 0.78, against 0.35 for the Jaccard rule.
inline constexpr double SHIFT_SEMANTIC_THRESHOLD = 0.5;
// Distinct content words each window needs for a meaningful comparison.
inline constexpr std::size_t SHIFT_MIN_WORDS = 8;
// Pending segment counts at which the interval shrinks to the minimum, or below which it
// stretches to the maximum.
inline constexpr int PRESSURE_HIGH = 8;
inline constexpr int PRESSURE_LOW = 3;
// Consecutive checkpoint failures before intelligence is declared offline.
inline constexpr int MAX_CONSECUTIVE_FAILURES = 3;
// Seconds before the next checkpoint after the 1st, 2nd, 3rd and later consecutive failures.
inline constexpr std::array<double, 4> RETRY_BACKOFF_S = {30.0, 60.0, 120.0, 300.0};
// A payload stays bounded even when transcription catches up after a long outage; one more row
// is read to learn whether the scheduler must continue on the next tick.
inline constexpr int DELIVERY_PAGE_SIZE = 300;
// The trailing transcript a guidance pass re-sends: the full dashboard state travels alongside,
// so a bounded recent window is enough context and keeps a long meeting inside the budget.
inline constexpr std::size_t GUIDANCE_MAX_SEGMENTS = 300;
// Lines of earlier transcript a cards pass carries as context.
inline constexpr std::size_t RECENT_CONTEXT_SEGMENTS = 24;
// Bounds on explicit note requests from the dashboard (code points; queued requests).
inline constexpr std::size_t NOTE_REQUEST_MAX_CHARS = 4000;
inline constexpr std::size_t MAX_QUEUED_NOTE_REQUESTS = 4;
// The default wall of the consolidation pass (meeting/agent/base.py), in seconds.
inline constexpr double CONSOLIDATION_TIMEOUT_CAP_S = 900.0;
// The per-block budget of the final transcript polish (meeting/finalization.py), in seconds.
inline constexpr double FINAL_POLISH_TIMEOUT_S = 180.0;

// The scheduler's tunables: the caller's card intervals (else the core's cadence, else the
// defaults above) and Python's module constants, which tests shorten as OpenWhisper's tests
// monkeypatch them. Seconds unless named otherwise.
struct SchedulerOptions {
    std::optional<double> base_interval_s;
    std::optional<double> min_interval_s;
    std::optional<double> max_interval_s;
    // How long the worker sleeps between ticks unless woken.
    double tick_s = 1.0;
    // Minimum spacing between topic-shift computations.
    double shift_check_spacing_s = 10.0;
    // How far behind the newest sent segment a time-window fetch reaches back: mic and loopback
    // chunks pass through one shared speech queue, so an earlier segment can be stored later.
    double refetch_window_s = 180.0;
    // The first polish waits this long after the last card checkpoint.
    double polish_initial_delay_s = 15.0;
    // Without a core cadence: polish after this many successful card checkpoints, or after
    // this long since the last polish; notes after this many, or this long.
    int polish_every = 6;
    double polish_min_interval_s = 45.0;
    int notes_every = 2;
    double notes_min_interval_s = 45.0;
    // How much recent transcript a notes payload may carry.
    std::size_t notes_max_segments = 300;
    // How long a final pass waits for a running checkpoint, once politely and again after
    // cancelling it.
    double consolidation_join_s = 10.0;
    // How long stop() waits for the worker to exit.
    double stop_join_s = 5.0;
    // How long a timed-out consolidation gets to unwind after its cancel.
    double cancel_grace_s = 5.0;
};

// Lowercased content words of `text`: runs of [a-z'] at least 4 long that are not stop words.
std::set<std::string, std::less<>> content_words(std::string_view text);

struct SchedulerProbe; // tests reach the internals through it

// Drives agent passes from transcript activity. The engine calls notify_segments per committed
// batch; the worker fires a checkpoint when enough time has passed for the segment pressure (or
// earlier on a topic shift), with the transcript not yet delivered.
// Affinity: thread-safe. Passes run on the worker thread, or on the caller's thread for
// run_due, run_consolidation and run_final_polish (which first stop the worker). The bookkeeping
// of passes belongs to whichever thread runs them; nothing else touches it while a worker runs.
class CheckpointScheduler {
public:
    // `on_health(online, error)` hears every change of the online state (exceptions it throws
    // are logged and swallowed). `clock` measures intervals and backoff, and must outlive the
    // scheduler, as must `host`.
    CheckpointScheduler(SchedulerHost& host, std::shared_ptr<AgentCore> agent, const Clock& clock,
                        std::function<void(bool online, std::string error)> on_health = {},
                        TopicJudge topic_judge = {}, SchedulerOptions options = {});
    // Stops and joins the worker. A pass still running is waited for: shut the core down first.
    ~CheckpointScheduler();
    CheckpointScheduler(const CheckpointScheduler&) = delete;
    CheckpointScheduler& operator=(const CheckpointScheduler&) = delete;

    // Starts the worker; does nothing while one runs. Reads the durable backlog first, so a
    // failed ledger read throws and leaves no worker behind.
    void start();
    // Stops periodic firing and settles queued note requests "Meeting agent stopped."; waits up
    // to stop_join_s for the worker. Does not cancel a pass in flight.
    void stop();
    // Runs one tick: what the worker does each time it wakes. Public for deterministic replay.
    void run_due();

    // Records `count` newly transcribed segments.
    void notify_segments(int count);
    // Asks for a prompt review of human context (a correction, a rewritten brief), even during a
    // quiet meeting; clears both failure backoffs once.
    void notify_guidance();
    // Queues an explicit dashboard note request on the worker. The future's result is the pass's
    // (failed "Meeting agent stopped." when the scheduler stops first). Throws
    // std::invalid_argument "Enter a note request of 1 to 4000 characters." and
    // std::runtime_error "The note agent is not available." or "The note agent already has
    // several requests queued.".
    std::future<AgentResult> request_note_adjustment(std::string text);
    // Completion runs on the scheduler's worker or on the caller that cancels queued work.
    void request_note_adjustment(std::string text, std::function<void(AgentResult)> completion,
                                 const CancelToken& permit = {});

    // Marks a meeting-to-date transcript as delivered to both the card and the notes passes, so
    // a scheduler restarted mid-meeting does not resend it. Throws what the ledger throws.
    void seed_sent_segments(const std::vector<Json>& segments);
    // The same over the host's whole transcript.
    void seed_sent_segments();

    // Stops new rolling passes without blocking the end of the meeting: the pass in flight is
    // left to run_consolidation.
    void prepare_for_end();
    // The blocking end-of-meeting pass over the complete transcript, bounded by `timeout_s`
    // (CONSOLIDATION_TIMEOUT_CAP_S when nullopt; the agent is cancelled on timeout). Agent writes
    // are revoked on every way out, then meeting_state::repair_meeting_state runs whenever the
    // pass ran. `progress` hears the core's progress sentences while it runs.
    ConsolidationOutcome run_consolidation(std::optional<double> timeout_s = std::nullopt,
                                           std::function<void(const std::string&)> progress = {});
    // The blocking transcript cleanup over the final segments through `polish` (timeout_s per
    // block). Never revokes agent writes, so run_consolidation can follow.
    ConsolidationOutcome
    run_final_polish(const PolishTranscript& polish, double timeout_s = FINAL_POLISH_TIMEOUT_S,
                     std::function<void(const std::string&, int, int)> progress = {});

    bool online() const;

private:
    friend struct SchedulerProbe;

    struct NoteRequest {
        std::string text;
        std::function<void(AgentResult)> completion;
        CancelToken permit;
    };
    // A note-taker pass about to run, and where its delivery resumes.
    struct NotesPass {
        CheckpointPayload payload;
        std::vector<Json> segments;
        bool guidance = false;
        bool more_delivery = false;
        std::int64_t delivery_seq = 0;
    };
    // A fetched batch, whether the ledger holds more, and the sequence that acknowledges it.
    struct Fetch {
        std::vector<Json> segments;
        bool more = false;
        std::int64_t seq = 0;
    };

    // scheduler.cpp
    double mono() const;
    double pace_polish_min() const;
    double pace_notes_min() const;
    int pace_polish_every() const;
    int pace_notes_every() const;
    double interval_for(int pending) const;
    double meeting_elapsed_s() const;
    bool checkpoint_due(int pending);
    void run_loop(std::shared_ptr<agents::Event> exited);
    bool join_worker(double timeout_s);
    void cancel_note_requests();
    void fire_note_request();
    std::optional<CheckpointPayload> build_payload(const std::vector<Json>& segments,
                                                   bool is_consolidation, bool is_polish = false,
                                                   bool is_notes = false);
    AgentResult run_agent_pass(const CheckpointPayload& payload, std::string_view failure_log);
    void fire();
    bool background_blocked() const;
    bool interactive_pending() const;
    void set_online(bool online, const std::string& error = {});

    // detail/scheduler_delivery.cpp
    Fetch delivery_page(std::string_view consumer);
    Fetch fetch_for_checkpoint(bool guidance);
    std::vector<Json> checkpoint_segments(const std::vector<Json>& fetched, bool guidance) const;
    double fetch_cursor_s() const;
    void mark_sent(const std::vector<Json>& segments);
    Fetch fetch_for_notes(bool guidance);
    std::vector<Json> notes_segments(const std::vector<Json>& fetched, bool guidance) const;
    void mark_notes_sent(const std::vector<Json>& segments);
    void prune_notes_sent();

    // detail/scheduler_topic.cpp
    bool detect_topic_shift();
    std::optional<bool> semantic_topic_shift(const std::string& older, const std::string& newer);

    // detail/scheduler_passes.cpp
    void checkpoint_failed(int claimed, bool guidance, const std::string& error,
                           const std::string& request_id = {});
    void record_checkpoint_success(const std::string& request_id, const std::vector<Json>& segments,
                                   const AgentResult& result);
    void record_failure(int claimed, const std::string& error, const std::string& request_id = {});
    void maybe_fire_polish();
    bool polish_due() const;
    void maybe_fire_notes();
    bool notes_due(bool guidance) const;
    std::optional<NotesPass> prepare_notes_pass(bool guidance, std::int64_t generation);
    void finish_notes_pass(const NotesPass& notes_pass, const AgentResult& result);
    void fail_notes_pass(const NotesPass& notes_pass, const AgentResult& result);
    void defer_notes_retry();

    // detail/scheduler_end.cpp
    bool stop_inflight_checkpoint();
    void revoke_writes();
    std::optional<ConsolidationOutcome> consolidation_precheck();
    std::optional<AgentResult> consolidate(const CheckpointPayload& payload,
                                           std::size_t segment_count, double timeout_s,
                                           const std::function<void(const std::string&)>& progress);
    void repair_after_consolidation(const std::vector<Json>& segments);

    SchedulerHost& host_;
    std::shared_ptr<AgentCore> agent_;
    const Clock& clock_;
    std::function<void(bool, std::string)> on_health_;
    TopicJudge topic_judge_;
    SchedulerOptions options_;
    std::optional<CheckpointCadence> cadence_;
    double base_interval_s_ = BASE_INTERVAL_S;
    double min_interval_s_ = MIN_INTERVAL_S;
    double max_interval_s_ = MAX_INTERVAL_S;

    // Python's _lock: what notify_*, request_note_adjustment and the worker share.
    mutable std::mutex mutex_;
    int pending_segments_ = 0;
    bool guidance_pending_ = false;
    bool notes_guidance_pending_ = false;
    std::int64_t guidance_generation_ = 0;
    double retry_not_before_ = 0.0;
    double notes_retry_not_before_ = 0.0;
    std::deque<NoteRequest> note_requests_;

    agents::Event wake_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> consolidating_{false};
    std::atomic<bool> online_{true};

    // The worker: its thread handle, and the flag it sets when its loop returns.
    mutable std::mutex thread_mutex_;
    std::thread thread_;
    std::shared_ptr<agents::Event> thread_exited_;

    // Pass bookkeeping (monotonic seconds of `clock_`; 0 is Python's "never").
    double last_fire_mono_ = 0.0;
    double started_mono_ = 0.0;
    double last_shift_check_mono_ = 0.0;
    // start_s of every segment handed to the card passes, pruned to the re-fetch window.
    std::map<std::string, double, std::less<>> sent_starts_;
    double max_sent_start_s_ = -1.0;
    int consecutive_failures_ = 0;
    int successful_checkpoints_ = 0;
    double last_polish_mono_ = 0.0;
    int polish_checkpoint_mark_ = 0;
    bool polish_retry_pending_ = false;
    double last_notes_mono_ = 0.0;
    int notes_checkpoint_mark_ = 0;
    std::map<std::string, double, std::less<>> notes_sent_starts_;
    double notes_max_sent_start_s_ = -1.0;
    bool delivery_enabled_ = false;
    std::int64_t card_cursor_seq_ = 0;
    std::int64_t notes_cursor_seq_ = 0;
    bool notes_resume_pending_ = false;
};

} // namespace fv::meeting_agent
