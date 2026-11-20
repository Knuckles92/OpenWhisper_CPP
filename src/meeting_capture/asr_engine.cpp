#include "meeting_capture/asr_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <format>
#include <system_error>
#include <utility>

#include "base/log.h"
#include "base/utf8.h"
#include "meeting_capture/asr_audio.h"
#include "meeting_capture/detail/files.h"
#include "meeting_capture/detail/wav_file.h"
#include "meeting_capture/segment_ids.h"
#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "speech/messages.h"

namespace fv::meeting {

using meeting_state::SpooledChunk;
using meeting_state::TranscriptSegment;

namespace {

constexpr log::Logger logger("meeting.asr.engine");

constexpr std::string_view kBehind =
    "Live transcription is behind; audio is saved and captions will catch up.";
constexpr std::string_view kCaughtUp = "Live transcription caught up.";
constexpr std::string_view kGaveUp = "Transcription failed after three attempts. Audio is saved; "
                                     "check the speech engine and retry this meeting.";
constexpr std::string_view kUnavailableStatus =
    "Speech engine unavailable; select or install a model, then retry saved audio";
constexpr std::string_view kUnavailableReport =
    "Speech engine unavailable. Select or install a model, then retry saved audio.";
constexpr std::string_view kUnexpectedStatus =
    "Unexpected ASR worker error; saved audio needs manual retry";
constexpr std::string_view kUnexpectedReport =
    "An unexpected transcription error occurred. Audio is saved; retry this meeting.";

std::chrono::duration<double> seconds(double value) {
    return std::chrono::duration<double>(std::max(0.0, value));
}

std::string normalized_language(std::string_view language) {
    std::string out = py::lower(py::strip(language));
    return out.empty() ? std::string("auto") : out;
}

// Python's truthiness-defaulted conversions of a chunk row: float(row.get(k) or 0) and the like.
double row_float(const Json& row, const char* key) {
    const auto found = row.find(key);
    return found != row.end() && found->is_number() ? found->get<double>() : 0.0;
}

std::int64_t row_int(const Json& row, const char* key) {
    const auto found = row.find(key);
    if (found == row.end() || !found->is_number()) {
        return 0;
    }
    return found->is_number_float() ? static_cast<std::int64_t>(found->get<double>())
                                    : found->get<std::int64_t>();
}

std::string row_text(const Json& row, const char* key) {
    const auto found = row.find(key);
    return found != row.end() && found->is_string() ? found->get<std::string>() : std::string();
}

// int(row["id"]): a row without one fails the refill.
std::int64_t row_id(const Json& row) {
    return row.at("id").get<std::int64_t>();
}

SpooledChunk chunk_from_row(const Json& row) {
    SpooledChunk chunk;
    chunk.chunk_id = row_id(row);
    chunk.meeting_id = row.at("meeting_id").get<std::string>();
    chunk.channel = row.at("channel").get<std::string>();
    chunk.seq = row.at("seq").get<std::int64_t>();
    chunk.file_path = row.at("file_path").get<std::string>();
    chunk.start_s = row.at("start_s").get<double>();
    chunk.duration_s = row.at("duration_s").get<double>();
    chunk.sample_rate = row.at("sample_rate").get<int>();
    return chunk;
}

std::string what_of(std::exception_ptr error) {
    try {
        std::rethrow_exception(std::move(error));
    } catch (const std::exception& caught) {
        return caught.what();
    } catch (...) {
        return "unknown error";
    }
}

// Python's str() of the OSErrors load_wav_int16 turns into a PermanentChunkError, or nullopt for
// another error (which is retried).
std::optional<std::string> unreadable_reason(const std::system_error& error,
                                             const std::filesystem::path& path) {
    if (error.code() == std::errc::no_such_file_or_directory) {
        return "[Errno 2] No such file or directory: " + py::repr(path_to_utf8(path));
    }
    if (error.code() == std::errc::permission_denied) {
        return "[Errno 13] Permission denied: " + py::repr(path_to_utf8(path));
    }
    return std::nullopt;
}

} // namespace

bool is_digital_silence(std::span<const std::int16_t> frames) {
    if (frames.empty()) {
        return true;
    }
    std::int32_t peak = 0;
    for (const std::int16_t sample : frames) {
        peak = std::max(peak, std::abs(static_cast<std::int32_t>(sample)));
    }
    return peak <= DIGITAL_SILENCE_PEAK;
}

std::vector<TranscriptSegment> chunk_segments(std::string_view meeting_id,
                                              const SpooledChunk& chunk,
                                              std::span<const Segment> decoded) {
    std::vector<TranscriptSegment> segments;
    std::int64_t ordinal = -1;
    for (const Segment& piece : decoded) {
        ++ordinal;
        std::string text = py::strip(piece.text);
        if (text.empty()) {
            continue;
        }
        TranscriptSegment segment;
        segment.segment_id = stable_segment_id(meeting_id, chunk.chunk_id, ordinal);
        segment.meeting_id = std::string(meeting_id);
        segment.chunk_id = chunk.chunk_id;
        segment.channel = chunk.channel;
        segment.start_s = chunk.start_s + piece.start;
        segment.end_s = chunk.start_s + piece.end;
        segment.text = std::move(text);
        segments.push_back(std::move(segment));
    }
    return segments;
}

MeetingAsrEngine::MeetingAsrEngine(speech::SpeechService& service,
                                   std::shared_ptr<meeting_state::MeetingRepository> repository,
                                   std::string meeting_id, std::string language, const Clock& clock,
                                   AsrCallbacks callbacks)
    : service_(service), repository_(std::move(repository)), meeting_id_(std::move(meeting_id)),
      language_(normalized_language(language)), clock_(clock), callbacks_(std::move(callbacks)) {}

MeetingAsrEngine::~MeetingAsrEngine() {
    stop();
    if (worker_.joinable()) {
        if (worker_.get_id() == std::this_thread::get_id()) {
            worker_.detach();
        } else {
            worker_.join();
        }
    }
    const std::lock_guard lock(preview_mutex_);
    previews_.clear();
}

void MeetingAsrEngine::set_timing(AsrTiming timing) {
    const std::lock_guard lock(mutex_);
    timing_ = std::move(timing);
}

double MeetingAsrEngine::now() const {
    return std::chrono::duration<double>(clock_.steady().time_since_epoch()).count();
}

bool MeetingAsrEngine::stop_requested() const {
    const std::lock_guard lock(mutex_);
    return stopping_ || stop_requested_;
}

CancelToken MeetingAsrEngine::stop_token() const {
    const std::lock_guard lock(mutex_);
    return stop_source_.token();
}

bool MeetingAsrEngine::wait_for_stop(double seconds_) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, seconds(seconds_), [this] { return stop_requested_; });
}

bool MeetingAsrEngine::backend_ready() const {
    if (!available_.load()) {
        return false;
    }
    // A slot that is loading again takes the job once it is ready; one that was released or
    // failed has no engine to decode with (Python's backend.is_available()).
    const speech::ReadyState state = service_.readiness(speech::Slot::Meeting).state;
    return state == speech::ReadyState::Ready || state == speech::ReadyState::Loading ||
           state == speech::ReadyState::Warming;
}

bool MeetingAsrEngine::load_backend(const CancelToken& cancel) {
    if (stop_requested()) {
        return false;
    }
    if (available_.load()) {
        return true;
    }
    double poll = 0.05;
    {
        const std::lock_guard lock(mutex_);
        poll = timing_.load_poll_s;
    }
    for (;;) {
        const speech::Readiness readiness = service_.readiness(speech::Slot::Meeting);
        // Warming is a loaded engine running its throwaway decode: it takes jobs.
        if (readiness.state == speech::ReadyState::Ready ||
            readiness.state == speech::ReadyState::Warming) {
            if (stop_requested()) {
                return false;
            }
            available_.store(true);
            logger.info("Meeting ASR engine ready: {}", readiness.device_info);
            return true;
        }
        if (readiness.state != speech::ReadyState::Loading) {
            std::string error = !readiness.message.empty()       ? readiness.message
                                : !readiness.device_info.empty() ? readiness.device_info
                                                                 : "Speech engine unavailable";
            {
                const std::lock_guard lock(mutex_);
                last_error_ = error;
            }
            logger.error("Meeting ASR engine failed to load ({}); engine unavailable", error);
            return false;
        }
        if (cancel.wait_for(std::chrono::duration_cast<std::chrono::milliseconds>(seconds(poll))) ||
            stop_requested()) {
            return false;
        }
    }
}

bool MeetingAsrEngine::is_available() const {
    return available_.load();
}

std::string MeetingAsrEngine::last_error() const {
    const std::lock_guard lock(mutex_);
    return last_error_;
}

void MeetingAsrEngine::start(ChunkResult on_chunk_result) {
    std::thread finished;
    {
        const std::lock_guard lock(mutex_);
        on_chunk_result_ = std::move(on_chunk_result);
        if (worker_.joinable() && !worker_done_) {
            return;
        }
        finished = std::move(worker_);
        stopping_ = false;
        stop_requested_ = false;
        stop_source_ = CancelSource();
        worker_done_ = false;
        worker_ = std::thread([this] { worker_loop(); });
    }
    if (finished.joinable()) {
        finished.join();
    }
}

bool MeetingAsrEngine::enqueue(const SpooledChunk& chunk) {
    return enqueue_chunk(chunk, false);
}

bool MeetingAsrEngine::enqueue_chunk(const SpooledChunk& chunk, bool from_refill) {
    bool overloaded = false;
    {
        const std::lock_guard lock(mutex_);
        if (stopping_ || stop_requested_) {
            return false;
        }
        if (queued_ids_.contains(chunk.chunk_id)) {
            return true;
        }
        // Once a chunk waits in SQLite, later live chunks go there too, so order is kept.
        if (queued_ids_.size() >= MAX_IN_MEMORY_CHUNKS || (deferred_known_ && !from_refill)) {
            deferred_known_ = true;
            refill_needed_ = true;
            overloaded = true;
        } else {
            queue_.push_back(chunk);
            queued_ids_.insert(chunk.chunk_id);
            enqueued_at_.try_emplace(chunk.chunk_id, now());
            ++outstanding_;
            changed_.notify_all();
        }
    }
    if (overloaded) {
        report_backlog(true);
        return false;
    }
    return true;
}

void MeetingAsrEngine::report_backlog(bool overloaded) {
    {
        const std::lock_guard lock(mutex_);
        if (backlog_overloaded_ == overloaded) {
            return;
        }
        backlog_overloaded_ = overloaded;
    }
    const std::string message(overloaded ? kBehind : kCaughtUp);
    if (overloaded) {
        logger.warning("Meeting ASR in-memory backlog reached {} chunks", MAX_IN_MEMORY_CHUNKS);
    } else {
        logger.info("Meeting ASR backlog recovered");
    }
    if (callbacks_.on_backlog_status) {
        try {
            callbacks_.on_backlog_status(message, overloaded);
        } catch (const std::exception& error) {
            logger.error_with(error, "Could not report meeting ASR backlog status");
        } catch (...) {
            logger.error("Could not report meeting ASR backlog status");
        }
    }
}

bool MeetingAsrEngine::drain(std::chrono::milliseconds timeout) {
    const double deadline = now() + std::max(0.0, std::chrono::duration<double>(timeout).count());
    double refill_retry = REFILL_RETRY_S;
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            refill_retry = timing_.refill_retry_s;
            while (outstanding_ > 0 || refilling_) {
                if (stopping_ || stop_requested_) {
                    return false;
                }
                const double remaining = deadline - now();
                if (remaining <= 0) {
                    const std::size_t outstanding = outstanding_;
                    lock.unlock();
                    logger.warning("ASR drain timed out with {} in-memory chunk(s) outstanding",
                                   outstanding);
                    return false;
                }
                changed_.wait_for(lock, seconds(remaining));
            }
            if (stopping_ || stop_requested_) {
                return false;
            }
            if (refill_needed_ && now() >= deadline) {
                return false;
            }
        }
        // A full in-memory queue may have left registered chunks in SQLite: look before claiming
        // the meeting is drained.
        const std::size_t added = requeue_pending();
        {
            const std::lock_guard lock(mutex_);
            if (added == 0 && outstanding_ == 0 && !refilling_ && !refill_needed_) {
                return true;
            }
        }
        const double remaining = deadline - now();
        if (remaining <= 0) {
            return false;
        }
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, seconds(std::min(refill_retry, remaining)));
    }
}

void MeetingAsrEngine::stop() {
    CancelSource source;
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        stop_requested_ = true;
        source = stop_source_;
    }
    // The running decode finishes on the speech thread and its result is dropped; queued
    // windows of it are dropped at once.
    source.cancel();
    changed_.notify_all();
    stop_preview();
    std::thread finished;
    {
        std::unique_lock lock(mutex_);
        if (worker_.joinable()) {
            if (worker_.get_id() == std::this_thread::get_id()) {
                return; // stopped from its own callback: the loop ends after it
            }
            const bool ended = changed_.wait_for(lock, seconds(timing_.stop_join_s),
                                                 [this] { return worker_done_; });
            if (!ended) {
                lock.unlock();
                logger.warning("Meeting ASR worker did not stop within 10s");
                // A decode or a database call may still use the queue's chunk: leave it all.
                return;
            }
            // Taken under the lock, so two threads stopping at once never join it twice.
            finished = std::move(worker_);
        }
    }
    if (finished.joinable()) {
        finished.join();
    }
    {
        const std::lock_guard lock(mutex_);
        queue_.clear();
        outstanding_ = 0;
        queued_ids_.clear();
        attempts_.clear();
        enqueued_at_.clear();
        queue_wait_warned_.clear();
        refilling_ = false;
        refill_needed_ = false;
        next_refill_at_ = 0.0;
        deferred_known_ = false;
        changed_.notify_all();
    }
    available_.store(false);
}

std::size_t MeetingAsrEngine::requeue_pending(std::optional<std::vector<Json>> rows) {
    std::size_t free = 0;
    std::vector<std::int64_t> excluded;
    {
        const std::lock_guard lock(mutex_);
        if (stopping_ || stop_requested_ || refilling_) {
            return 0;
        }
        if (refill_needed_ && now() < next_refill_at_) {
            return 0;
        }
        refilling_ = true;
        free = MAX_IN_MEMORY_CHUNKS - std::min(MAX_IN_MEMORY_CHUNKS, queued_ids_.size());
        excluded.assign(queued_ids_.begin(), queued_ids_.end());
    }
    std::size_t requeued = 0;
    bool has_extra = false;
    bool failed = false;
    try {
        const std::vector<Json> pending = pending_refill_rows(std::move(rows), free, excluded);
        has_extra = pending.size() > free;
        for (std::size_t i = 0; i < std::min(free, pending.size()); ++i) {
            const SpooledChunk chunk = chunk_from_row(pending[i]);
            {
                const std::lock_guard lock(mutex_);
                if (queued_ids_.contains(chunk.chunk_id)) {
                    continue;
                }
                attempts_[chunk.chunk_id] = row_int(pending[i], "asr_attempts");
            }
            if (enqueue_chunk(chunk, true)) {
                ++requeued;
            } else {
                has_extra = true;
            }
        }
    } catch (const std::exception& error) {
        failed = true;
        has_extra = true;
        logger.error_with(error,
                          std::format("Could not refill saved ASR chunks for {}", meeting_id_));
    } catch (...) {
        failed = true;
        has_extra = true;
        logger.error("Could not refill saved ASR chunks for {}", meeting_id_);
    }
    const bool quiet = finish_refill(has_extra, failed);
    if (has_extra) {
        report_backlog(true);
    } else if (quiet) {
        report_backlog(false);
    }
    if (requeued > 0) {
        logger.info("Queued {} saved ASR chunk(s) for meeting {}", requeued, meeting_id_);
    }
    return requeued;
}

std::vector<Json> MeetingAsrEngine::pending_refill_rows(std::optional<std::vector<Json>> rows,
                                                        std::size_t free,
                                                        const std::vector<std::int64_t>& excluded) {
    if (!rows) {
        rows = repository_->get_pending_chunks(meeting_id_, static_cast<std::int64_t>(free) + 1,
                                               excluded);
    }
    std::vector<Json> out;
    for (Json& row : *rows) {
        if (std::ranges::find(excluded, row_id(row)) == excluded.end()) {
            out.push_back(std::move(row));
        }
    }
    std::ranges::stable_sort(out, [](const Json& a, const Json& b) {
        const double a_start = row_float(a, "start_s");
        const double b_start = row_float(b, "start_s");
        if (a_start != b_start) {
            return a_start < b_start;
        }
        const std::int64_t a_seq = row_int(a, "seq");
        const std::int64_t b_seq = row_int(b, "seq");
        if (a_seq != b_seq) {
            return a_seq < b_seq;
        }
        const std::string a_channel = row_text(a, "channel");
        const std::string b_channel = row_text(b, "channel");
        if (a_channel != b_channel) {
            return a_channel < b_channel;
        }
        return row_id(a) < row_id(b);
    });
    return out;
}

bool MeetingAsrEngine::finish_refill(bool has_extra, bool failed) {
    const std::lock_guard lock(mutex_);
    refilling_ = false;
    refill_needed_ = has_extra;
    next_refill_at_ = failed ? now() + timing_.refill_retry_s : 0.0;
    deferred_known_ = has_extra;
    const bool quiet =
        !has_extra && outstanding_ <= static_cast<std::size_t>(FAST_MODE_BACKLOG_CHUNKS);
    changed_.notify_all();
    return quiet;
}

void MeetingAsrEngine::worker_loop() {
    detail::set_thread_name("fv-meeting-asr");
    for (;;) {
        std::optional<SpooledChunk> item;
        bool refill = false;
        {
            std::unique_lock lock(mutex_);
            if (stop_requested_) {
                break;
            }
            changed_.wait_for(lock, seconds(timing_.refill_retry_s),
                              [this] { return stop_requested_ || !queue_.empty(); });
            if (stop_requested_) {
                break;
            }
            if (queue_.empty()) {
                refill = refill_needed_;
            } else {
                item = std::move(queue_.front());
                queue_.pop_front();
            }
        }
        if (!item) {
            if (refill) {
                requeue_pending();
            }
            continue;
        }
        try {
            process_chunk(*item);
        } catch (...) {
            const std::exception_ptr error = std::current_exception();
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& caught) {
                logger.error_with(
                    caught, std::format("Unexpected ASR worker error on chunk {}", item->chunk_id));
            } catch (...) {
                logger.error("Unexpected ASR worker error on chunk {}", item->chunk_id);
            }
            set_status(item->chunk_id, "blocked", std::string(kUnexpectedStatus));
            report_chunk_failure(item->chunk_id, std::string(kUnexpectedReport));
        }
        {
            const std::lock_guard lock(mutex_);
            queued_ids_.erase(item->chunk_id);
            attempts_.erase(item->chunk_id);
            enqueued_at_.erase(item->chunk_id);
            queue_wait_warned_.erase(item->chunk_id);
        }
        // Outstanding stays above zero through the refill, so drain() cannot report success
        // between a dequeue and the saved chunks behind it.
        if (!stop_requested()) {
            requeue_pending();
        }
        bool quiet = false;
        {
            const std::lock_guard lock(mutex_);
            --outstanding_;
            quiet = !deferred_known_ &&
                    outstanding_ <= static_cast<std::size_t>(FAST_MODE_BACKLOG_CHUNKS);
            changed_.notify_all();
        }
        if (quiet) {
            report_backlog(false);
        }
    }
    logger.debug("Meeting ASR worker exited");
    {
        const std::lock_guard lock(mutex_);
        worker_done_ = true;
    }
    changed_.notify_all();
}

void MeetingAsrEngine::process_chunk(const SpooledChunk& chunk) {
    std::int64_t attempts = 0;
    {
        const std::lock_guard lock(mutex_);
        const auto found = attempts_.find(chunk.chunk_id);
        attempts = found == attempts_.end() ? 0 : found->second;
    }
    int remote_failures = 0;
    while (!stop_requested()) {
        if (!backend_ready()) {
            block_on_unavailable_engine(chunk);
            return;
        }
        ++attempts;
        {
            const std::lock_guard lock(mutex_);
            attempts_[chunk.chunk_id] = attempts;
        }
        if (attempt_chunk(chunk, attempts, remote_failures) == Attempt::Done) {
            return;
        }
    }
}

MeetingAsrEngine::Attempt MeetingAsrEngine::attempt_chunk(const SpooledChunk& chunk,
                                                          std::int64_t& attempts,
                                                          int& remote_failures) {
    try {
        set_status(chunk.chunk_id, "processing");
        log_queue_wait(chunk);
        std::vector<TranscriptSegment> segments = transcribe_chunk(chunk);
        ChunkResult callback;
        {
            const std::lock_guard lock(mutex_);
            callback = on_chunk_result_;
        }
        if (!callback) {
            throw std::runtime_error("No durable ASR result callback is registered");
        }
        // The callback is the durability boundary; a failed commit is retried.
        callback(chunk, std::move(segments));
        if (remote_failures && callbacks_.on_connection_status) {
            callbacks_.on_connection_status(
                callbacks_.connected_message.empty()
                    ? std::string("Connected to the meeting speech host.")
                    : callbacks_.connected_message,
                true);
        }
        return Attempt::Done;
    } catch (const PermanentChunkError& error) {
        logger.error("Saved ASR chunk {} needs repair: {}", chunk.chunk_id, error.what());
        set_status(chunk.chunk_id, "blocked", error.what());
        report_chunk_failure(chunk.chunk_id, error.what());
        return Attempt::Done;
    } catch (const std::exception& error) {
        if (stop_requested()) {
            defer_after_stop(chunk, error.what());
            return Attempt::Done;
        }
        if (const auto* remote = dynamic_cast<const EngineError*>(&error);
            remote && remote->retry != EngineError::Retry::None) {
            // The database counted this attempt when it became processing. The durable row
            // returns to pending, including if Stop interrupts the wait below.
            repository_->defer_chunk_after_connection_failure(chunk.chunk_id, error.what());
            --attempts;
            {
                const std::lock_guard lock(mutex_);
                attempts_[chunk.chunk_id] = attempts;
            }
            const bool busy = remote->retry == EngineError::Retry::Capacity;
            if (callbacks_.on_connection_status) {
                callbacks_.on_connection_status(
                    std::string(error.what()) +
                        (busy ? " Audio is saved locally; waiting for host capacity."
                              : " Audio is saved locally; retrying the remote connection."),
                    busy);
            }
            // asr/engine.py _remote_retry_delay.
            std::vector<double> backoff;
            double busy_min = REMOTE_BUSY_MIN_RETRY_S;
            {
                const std::lock_guard lock(mutex_);
                backoff = timing_.remote_retry_backoff_s;
                busy_min = timing_.remote_busy_min_retry_s;
            }
            const double longest = backoff.empty() ? 0.0 : backoff.back();
            const double fallback =
                backoff.empty() ? 0.0
                                : backoff[std::min(static_cast<std::size_t>(remote_failures),
                                                   backoff.size() - 1)];
            remote_failures = std::min(remote_failures + 1, 1 << 20);
            const auto delay = remote->retry_after_s;
            const double wait = delay && std::isfinite(*delay) && *delay >= 0
                                    ? std::min(longest, std::max(busy_min, *delay))
                                    : fallback;
            return wait_for_stop(wait) ? Attempt::Done : Attempt::Retry;
        }
        logger.error_with(error, std::format("Transcription failed for chunk {} (attempt {}/{})",
                                             chunk.chunk_id, attempts, MAX_ATTEMPTS));
        return retry_after_failure(chunk, error.what(), attempts) ? Attempt::Retry : Attempt::Done;
    } catch (...) {
        if (stop_requested()) {
            defer_after_stop(chunk, what_of(std::current_exception()));
            return Attempt::Done;
        }
        logger.error("Transcription failed for chunk {} (attempt {}/{})", chunk.chunk_id, attempts,
                     MAX_ATTEMPTS);
        return retry_after_failure(chunk, what_of(std::current_exception()), attempts)
                   ? Attempt::Retry
                   : Attempt::Done;
    }
}

bool MeetingAsrEngine::retry_after_failure(const SpooledChunk& chunk, const std::string& error,
                                           std::int64_t attempts) {
    set_status(chunk.chunk_id, "failed", error);
    if (attempts >= MAX_ATTEMPTS) {
        logger.error("Giving up on chunk {} after {} attempts; audio remains saved", chunk.chunk_id,
                     attempts);
        report_chunk_failure(chunk.chunk_id, std::string(kGaveUp));
        return false;
    }
    std::vector<double> backoff;
    {
        const std::lock_guard lock(mutex_);
        backoff = timing_.retry_backoff_s;
    }
    const double delay =
        backoff.empty()
            ? 0.0
            : backoff[std::min(static_cast<std::size_t>(attempts - 1), backoff.size() - 1)];
    return !wait_for_stop(delay);
}

void MeetingAsrEngine::defer_after_stop(const SpooledChunk& chunk, const std::string& error) {
    // stop() cancels the running decode, where Python's join let it finish and commit: the cut
    // short attempt is refunded and the chunk stays pending for recovery, never failed or given
    // up because the meeting stopped.
    logger.info("Meeting ASR stopped during chunk {}; its audio stays saved for recovery",
                chunk.chunk_id);
    try {
        repository_->defer_chunk_after_connection_failure(chunk.chunk_id, error);
    } catch (const std::exception& caught) {
        logger.error_with(caught,
                          std::format("Could not return chunk {} to pending", chunk.chunk_id));
    } catch (...) {
        logger.error("Could not return chunk {} to pending", chunk.chunk_id);
    }
}

void MeetingAsrEngine::block_on_unavailable_engine(const SpooledChunk& chunk) {
    set_status(chunk.chunk_id, "blocked", std::string(kUnavailableStatus));
    report_chunk_failure(chunk.chunk_id, std::string(kUnavailableReport));
    logger.warning("ASR engine unavailable; chunk {} needs manual retry", chunk.chunk_id);
}

void MeetingAsrEngine::log_queue_wait(const SpooledChunk& chunk) {
    double wait_s = 0.0;
    std::size_t behind = 0;
    {
        const std::lock_guard lock(mutex_);
        const auto found = enqueued_at_.find(chunk.chunk_id);
        if (found == enqueued_at_.end() || queue_wait_warned_.contains(chunk.chunk_id)) {
            return;
        }
        wait_s = now() - found->second;
        if (wait_s < QUEUE_WAIT_WARNING_S) {
            return;
        }
        queue_wait_warned_.insert(chunk.chunk_id);
        behind = outstanding_ > 0 ? outstanding_ - 1 : 0;
    }
    logger.warning("Meeting ASR chunk {} waited {}s (queued behind it: {})", chunk.chunk_id,
                   py::format_fixed(wait_s, 1), behind);
}

std::vector<TranscriptSegment> MeetingAsrEngine::transcribe_chunk(const SpooledChunk& chunk) {
    const std::filesystem::path path = path_from_utf8(chunk.file_path);
    const auto unreadable = [](const std::string& reason) {
        return PermanentChunkError("Saved audio cannot be read (" + reason +
                                   "); restore or replace the WAV, then retry");
    };
    std::pair<std::vector<std::int16_t>, int> loaded;
    try {
        loaded = detail::load_wav_int16(path);
    } catch (const std::system_error& error) {
        if (const auto reason = unreadable_reason(error, path)) {
            throw unreadable(*reason);
        }
        throw;
    } catch (const detail::WaveError& error) {
        throw unreadable(error.what());
    } catch (const py::ValueError& error) {
        throw unreadable(error.what());
    }
    const auto& [frames, sample_rate] = loaded;
    if (is_digital_silence(frames)) {
        logger.debug("Skipping digitally silent meeting chunk {}", chunk.chunk_id);
        return {};
    }
    const auto invalid = [](const std::string& reason) {
        return PermanentChunkError("Saved audio has invalid sample data (" + reason +
                                   "); repair the WAV, then retry");
    };
    std::vector<float> audio;
    try {
        audio = prepare_for_decoder(frames, sample_rate);
    } catch (const py::PyError& error) {
        if (error.type_name() == "ValueError" || error.type_name() == "ZeroDivisionError") {
            throw invalid(error.what());
        }
        throw;
    } catch (const std::invalid_argument& error) {
        throw invalid(error.what()); // numpy's ValueError from the FFT
    }
    if (audio.empty()) {
        return {};
    }

    speech::DecodeJob job;
    speech::AudioClip clip;
    clip.end = audio.size();
    clip.samples = std::make_shared<const std::vector<float>>(std::move(audio));
    clip.offset_s = chunk.start_s;
    job.input = std::move(clip);
    job.options.language = language_;
    job.lane = speech::Lane::Meeting;
    job.slot = speech::Slot::Meeting;
    job.cancel = stop_token();
    const speech::DecodeResult result = service_.run(std::move(job));
    return chunk_segments(meeting_id_, chunk, result.transcript.segments);
}

void MeetingAsrEngine::set_status(std::int64_t chunk_id, std::string_view status,
                                  std::optional<std::string> error) {
    try {
        if (error) {
            repository_->set_chunk_status(chunk_id, status, std::string_view(*error));
        } else {
            repository_->set_chunk_status(chunk_id, status, std::nullopt);
        }
    } catch (const std::exception& caught) {
        logger.error_with(caught,
                          std::format("Could not set chunk {} status to '{}'", chunk_id, status));
    } catch (...) {
        logger.error("Could not set chunk {} status to '{}'", chunk_id, status);
    }
}

void MeetingAsrEngine::report_chunk_failure(std::int64_t chunk_id, const std::string& reason) {
    if (!callbacks_.on_chunk_failure) {
        return;
    }
    try {
        callbacks_.on_chunk_failure(std::format("Saved audio chunk {}: {}", chunk_id, reason));
    } catch (const std::exception& error) {
        logger.error_with(error, "Could not report terminal ASR failure");
    } catch (...) {
        logger.error("Could not report terminal ASR failure");
    }
}

bool MeetingAsrEngine::preview_busy() const {
    const std::lock_guard lock(mutex_);
    return outstanding_ > 0 || stopping_;
}

void MeetingAsrEngine::start_preview(PreviewCallback callback) {
    const std::lock_guard lock(preview_mutex_);
    if (preview_.load() != nullptr || !available_.load()) {
        return;
    }
    // meeting/asr/engine.py start_preview: only a Parakeet model previews, loaded here or on a
    // paired host (Capabilities::live_preview); an engine that takes whole files has none.
    if (service_.accepts_files(speech::Slot::Meeting) ||
        !service_.live_preview(speech::Slot::Meeting)) {
        return;
    }
    PreviewOptions options;
    {
        const std::lock_guard state(mutex_);
        options = timing_.preview;
    }
    auto decode = [this](std::string_view channel, std::span<const float> samples,
                         const CancelToken& stop) -> std::optional<std::string> {
        // Durable work first: never queue speculative text behind it.
        if (preview_busy()) {
            return std::nullopt;
        }
        speech::DecodeJob job;
        speech::AudioClip clip;
        clip.end = samples.size();
        clip.samples = std::make_shared<const std::vector<float>>(samples.begin(), samples.end());
        job.input = std::move(clip);
        job.options.language = language_;
        job.lane = speech::Lane::Preview;
        job.slot = speech::Slot::Meeting;
        job.coalesce_key = "meeting-preview:" + std::string(channel);
        job.cancel = stop;
        try {
            return service_.run(std::move(job)).transcript.text;
        } catch (const EngineError& error) {
            if (stop.cancelled() ||
                std::string_view(error.what()) == speech::kTranscriptionCanceled) {
                return std::nullopt;
            }
            throw;
        }
    };
    auto preview = std::make_unique<WindowSpeechPreview>(
        std::move(decode), std::move(callback), [this] { return preview_busy(); }, clock_, options);
    preview_.store(preview.get());
    previews_.push_back(std::move(preview));
}

void MeetingAsrEngine::feed_preview(const CaptureBlock& block, double start_s) noexcept {
    if (WindowSpeechPreview* preview = preview_.load(std::memory_order_acquire)) {
        preview->feed(block, start_s);
    }
}

void MeetingAsrEngine::stop_preview() {
    const std::lock_guard lock(preview_mutex_);
    if (WindowSpeechPreview* preview = preview_.exchange(nullptr)) {
        preview->stop();
    }
}

std::vector<TranscriptSegment>
MeetingAsrEngine::transcribe_offline_session(const std::filesystem::path& spool_dir,
                                             const std::vector<Json>& chunks,
                                             OfflineProgress progress, const CancelToken& cancel) {
    if (!available_.load()) {
        return {};
    }
    const WindowDecoder decode = background_window_decoder(service_, language_, cancel);
    MeetingSessionsOptions options;
    options.progress = std::move(progress);
    return transcribe_meeting_sessions(decode, spool_dir, meeting_id_, chunks, options);
}

std::size_t MeetingAsrEngine::outstanding() const {
    const std::lock_guard lock(mutex_);
    return outstanding_;
}

std::function<void(const CaptureBlock&)> make_preview_feed(MeetingAsrEngine& engine,
                                                           const MeetingClock& clock) {
    return [&engine, &clock](const CaptureBlock& block) {
        engine.feed_preview(block, clock.meeting_time(block.t_mono));
    };
}

} // namespace fv::meeting
