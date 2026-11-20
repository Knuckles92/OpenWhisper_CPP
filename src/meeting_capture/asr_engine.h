#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/json.h"
#include "meeting_capture/capture_block.h"
#include "meeting_capture/meeting_clock.h"
#include "meeting_capture/offline.h"
#include "meeting_capture/window_preview.h"
#include "meeting_state/repository.h"
#include "meeting_state/types.h"
#include "speech/service.h"

// meeting/asr/engine.py MeetingAsrEngine: transcribes a meeting's spooled chunks on the speech
// service's Meeting lane (Slot::Meeting, which falls back to the dictation engine while the
// meeting has no model of its own). One worker thread "fv-meeting-asr" takes chunks from a
// bounded in-memory queue; the registered WAV and its SQLite row stay durable when that queue is
// full, and the worker refills from SQLite in bounded pages. Failures retry with backoff up to
// MAX_ATTEMPTS; anything unfinished survives for recovery through requeue_pending. Whisper's
// beam sizes, prompts, language votes and hallucination scores and the remote host branches are
// not ported: Parakeet ignores the first, reports none of the scores, and remote meetings are
// M9's.
namespace fv::meeting {

// Decode attempts per chunk before it is given up (status failed, attempts spent).
inline constexpr std::int64_t MAX_ATTEMPTS = meeting_state::MAX_CHUNK_ATTEMPTS;
// Chunk references held in memory, the one being decoded included; the rest wait in SQLite.
inline constexpr std::size_t MAX_IN_MEMORY_CHUNKS = 64;
// Pauses before the second and third attempts, so a failing model or database is not hammered.
inline constexpr double RETRY_BACKOFF_FIRST_S = 0.5;
inline constexpr double RETRY_BACKOFF_SECOND_S = 2.0;
// SQLite is checked again this long after a failed refill.
inline constexpr double REFILL_RETRY_S = 5.0;
// A disconnected remote host spends no decode attempt: these pauses, one per consecutive
// failure (the last repeats), unless the host says how long to wait, which is held to
// [REMOTE_BUSY_MIN_RETRY_S, the last pause].
inline constexpr double REMOTE_RETRY_BACKOFF_S[] = {5.0, 10.0, 20.0, 40.0, 60.0};
inline constexpr double REMOTE_BUSY_MIN_RETRY_S = 0.5;
// Outstanding chunks at or below which the backlog counts as caught up.
inline constexpr std::int64_t FAST_MODE_BACKLOG_CHUNKS = 3;
// A chunk that waited this long in the queue is logged once.
inline constexpr double QUEUE_WAIT_WARNING_S = 15.0;
// Peak int16 amplitude at or below which a chunk is digital silence: committed with no
// segments and never decoded. Conservative: quiet microphones carry usable speech at low levels,
// while idle loopback capture gives exact zeros.
inline constexpr int DIGITAL_SILENCE_PEAK = 8;
inline constexpr double ASR_STOP_JOIN_S = 10.0;

// The saved WAV needs repair or replacement before ASR can retry: the chunk is blocked.
class PermanentChunkError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct AsrCallbacks {
    // ("Live transcription is behind; audio is saved and captions will catch up.", true) when
    // the in-memory queue fills, ("Live transcription caught up.", false) once it recovers.
    std::function<void(const std::string& message, bool overloaded)> on_backlog_status;
    // "Saved audio chunk {id}: {reason}" for a chunk that needs the user's retry.
    std::function<void(const std::string& message)> on_chunk_failure;
    std::function<void(const std::string& message, bool connected)> on_connection_status;
    // What on_connection_status says once a remote host decodes again after an outage
    // ("Transcribing on <host>", asr/engine.py _check_remote_ready); empty reads "Connected to
    // the meeting speech host.".
    std::string connected_message;
};

// Pacing; tests shorten it.
struct AsrTiming {
    std::vector<double> retry_backoff_s = {RETRY_BACKOFF_FIRST_S, RETRY_BACKOFF_SECOND_S};
    double refill_retry_s = REFILL_RETRY_S;
    std::vector<double> remote_retry_backoff_s = {std::begin(REMOTE_RETRY_BACKOFF_S),
                                                  std::end(REMOTE_RETRY_BACKOFF_S)};
    double remote_busy_min_retry_s = REMOTE_BUSY_MIN_RETRY_S;
    double stop_join_s = ASR_STOP_JOIN_S;
    // How often load_backend looks at the speech service's readiness.
    double load_poll_s = 0.05;
    PreviewOptions preview;
};

// Called on the worker thread for every transcribed chunk, silent ones included. It is the
// durability boundary: it stores the segments and marks the chunk done, or throws so the chunk is
// retried.
using ChunkResult = std::function<void(const meeting_state::SpooledChunk&,
                                       std::vector<meeting_state::TranscriptSegment>)>;

// _is_digital_silence: empty, or a peak absolute value (through int32) of at most
// DIGITAL_SILENCE_PEAK.
bool is_digital_silence(std::span<const std::int16_t> frames);

// The end of _transcribe_chunk: the decoder's segments (seconds from the chunk's start) on the
// meeting clock, with stable ids. Text is stripped and empty segments are dropped; every decoder
// segment takes an ordinal, so the ids of the others never move.
std::vector<meeting_state::TranscriptSegment>
chunk_segments(std::string_view meeting_id, const meeting_state::SpooledChunk& chunk,
               std::span<const Segment> decoded);

// Affinity: thread-safe. The worker "fv-meeting-asr" and the preview thread "fv-meeting-preview"
// block on the speech service, never on "fv-speech" itself; callbacks run on them without a lock
// held. The service, the clock and the callbacks must outlive the engine.
class MeetingAsrEngine {
public:
    // `language` is "auto" or an ISO-639-1 code; it is stripped and lower-cased, and an empty one
    // becomes "auto", because the speech service reads an empty language as the dictation
    // language. Nothing loads until load_backend().
    MeetingAsrEngine(speech::SpeechService& service,
                     std::shared_ptr<meeting_state::MeetingRepository> repository,
                     std::string meeting_id, std::string language, const Clock& clock,
                     AsrCallbacks callbacks = {});
    // Stops, then waits for the worker and preview threads however long their decodes take.
    ~MeetingAsrEngine();
    MeetingAsrEngine(const MeetingAsrEngine&) = delete;
    MeetingAsrEngine& operator=(const MeetingAsrEngine&) = delete;

    // Before start().
    void set_timing(AsrTiming timing);

    // Waits until the Meeting slot is Ready or Warming (true) or reports another state than
    // Loading (false, with last_error), giving up when `cancel` or stop() comes first. Blocks;
    // never on the GUI thread.
    bool load_backend(const CancelToken& cancel);
    bool is_available() const;
    std::string last_error() const;
    const std::string& meeting_id() const { return meeting_id_; }
    // The language every meeting decode carries: the configured code, or "auto".
    const std::string& language() const { return language_; }

    // Starts the worker (again after stop()); a running worker keeps running with the new
    // callback.
    void start(ChunkResult on_chunk_result);
    // Queues a registered chunk without blocking capture. False when stopping, or when the queue
    // is full or earlier chunks were deferred to SQLite (the chunk then waits there, in order,
    // for the worker's refill); true when it is queued or already was.
    bool enqueue(const meeting_state::SpooledChunk& chunk);
    // Refills one bounded page of pending rows from SQLite (or from `rows`), in meeting-time
    // order; returns how many were queued. Nothing while stopping, refilling, or within
    // refill_retry_s of a failed refill.
    std::size_t requeue_pending(std::optional<std::vector<Json>> rows = std::nullopt);
    // Waits until every queued chunk has finished or given up and SQLite holds no pending chunk
    // the queue could take; false on timeout or stop.
    bool drain(std::chrono::milliseconds timeout);
    // Stops the preview and the worker (cancelling its decode, joining up to stop_join_s) and
    // forgets the queue; unfinished chunks stay pending in SQLite. When the worker does not end
    // in time, the queue is left as it is.
    void stop();

    // Starts the live caption preview when the engine is available and its backend previews
    // (Capabilities::live_preview: Parakeet, local or paired; a backend that takes whole files
    // has none). Previews are skipped while a chunk is
    // outstanding or the engine stops.
    void start_preview(PreviewCallback callback);
    // Copies a capture block (meeting time start_s) into the preview; nothing without one.
    // Never blocks: called on device threads.
    void feed_preview(const CaptureBlock& block, double start_s) noexcept;
    void stop_preview();

    // The offline re-decode of the session audio (offline.h) with this engine's language on the
    // Background lane; [] when the engine is not available. `chunks` are the meeting's chunk
    // rows, for channels whose session WAV is missing. Throws OfflineWindowError.
    std::vector<meeting_state::TranscriptSegment>
    transcribe_offline_session(const std::filesystem::path& spool_dir,
                               const std::vector<Json>& chunks, OfflineProgress progress,
                               const CancelToken& cancel);

    // Chunks queued or being decoded.
    std::size_t outstanding() const;

private:
    enum class Attempt { Done, Retry };

    double now() const;
    bool stop_requested() const;
    bool backend_ready() const;
    bool enqueue_chunk(const meeting_state::SpooledChunk& chunk, bool from_refill);
    void report_backlog(bool overloaded);
    std::vector<Json> pending_refill_rows(std::optional<std::vector<Json>> rows, std::size_t free,
                                          const std::vector<std::int64_t>& excluded);
    bool finish_refill(bool has_extra, bool failed);
    void worker_loop();
    void process_chunk(const meeting_state::SpooledChunk& chunk);
    Attempt attempt_chunk(const meeting_state::SpooledChunk& chunk, std::int64_t& attempts,
                          int& remote_failures);
    bool retry_after_failure(const meeting_state::SpooledChunk& chunk, const std::string& error,
                             std::int64_t attempts);
    void block_on_unavailable_engine(const meeting_state::SpooledChunk& chunk);
    void defer_after_stop(const meeting_state::SpooledChunk& chunk, const std::string& error);
    void log_queue_wait(const meeting_state::SpooledChunk& chunk);
    std::vector<meeting_state::TranscriptSegment>
    transcribe_chunk(const meeting_state::SpooledChunk& chunk);
    void set_status(std::int64_t chunk_id, std::string_view status,
                    std::optional<std::string> error = std::nullopt);
    void report_chunk_failure(std::int64_t chunk_id, const std::string& reason);
    // Waits up to `seconds` for stop(); true when stop came.
    bool wait_for_stop(double seconds);
    CancelToken stop_token() const;
    bool preview_busy() const;

    speech::SpeechService& service_;
    const std::shared_ptr<meeting_state::MeetingRepository> repository_;
    const std::string meeting_id_;
    const std::string language_;
    const Clock& clock_;
    const AsrCallbacks callbacks_;
    AsrTiming timing_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    ChunkResult on_chunk_result_;
    std::atomic<bool> available_{false};
    std::string last_error_;
    bool stopping_ = false;
    bool stop_requested_ = false;
    CancelSource stop_source_;
    std::deque<meeting_state::SpooledChunk> queue_;
    std::set<std::int64_t> queued_ids_;
    std::size_t outstanding_ = 0;
    bool refilling_ = false;
    bool refill_needed_ = false;
    double next_refill_at_ = 0.0;
    bool deferred_known_ = false;
    bool backlog_overloaded_ = false;
    std::map<std::int64_t, std::int64_t> attempts_;
    std::map<std::int64_t, double> enqueued_at_;
    std::set<std::int64_t> queue_wait_warned_;
    std::thread worker_;
    bool worker_done_ = true;

    std::mutex preview_mutex_;
    // The live preview; read by feed_preview on device threads without a lock. A stopped preview
    // is kept until the engine is destroyed, so a feed that raced its stop still reaches a live
    // object.
    std::atomic<WindowSpeechPreview*> preview_{nullptr};
    std::vector<std::unique_ptr<WindowSpeechPreview>> previews_; // guarded by preview_mutex_
};

// The capture's preview feed for this engine: each block's meeting time is the clock's reading
// of its monotonic timestamp (capture_session.py's router passes clock.meeting_time(t_mono)).
std::function<void(const CaptureBlock&)> make_preview_feed(MeetingAsrEngine& engine,
                                                           const MeetingClock& clock);

} // namespace fv::meeting
