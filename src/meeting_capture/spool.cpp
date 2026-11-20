#include "meeting_capture/spool.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <format>
#include <string_view>
#include <system_error>

#include "base/log.h"
#include "base/utf8.h"
#include "dsp/fft_resample.h"
#include "meeting_capture/detail/block_ring.h"
#include "meeting_capture/detail/cut_scanner.h"
#include "meeting_capture/detail/files.h"
#include "meeting_capture/detail/wav_file.h"
#include "meeting_capture/session_audio.h"
#include "pyport/errors.h"
#include "pyport/number.h"

namespace fv::meeting {

namespace {

constexpr log::Logger logger("meeting.capture.spool");

// How often the writer thread looks for queued blocks: feed() runs on the device thread, which may
// not signal (no syscalls there).
constexpr auto kWriterPoll = std::chrono::milliseconds(10);
constexpr auto kRegisterRetryDelay = std::chrono::milliseconds(100);
// Timeline corrections logged at INFO before the rest go to DEBUG.
constexpr int kMaxGapLogs = 5;
// Seconds of session audio between session JSON rewrites while capturing.
constexpr double kSessionMetaEveryS = 5.0;

// The audio storage failures, first one wins. Their sentences open with the channel.
enum class Health : int {
    None = 0,
    Dropped,
    WriteFailed,
    Crashed,
    RemainderFailed,
    SessionFinalizeFailed,
    DidNotFinish,
    SessionFlush,
    MetaWrite,
    SessionClose,
    Convert,
    SessionWrite,
    ChunkWrite,
    Register,
};

constexpr std::string_view kIncomplete = "This meeting's audio is incomplete.";
constexpr std::string_view kKept = "Partial meeting audio was kept for recovery.";

std::string health_sentence(Health health, std::string_view channel) {
    switch (health) {
    case Health::Dropped:
        return std::format("{} audio blocks were dropped because recording storage fell behind. {}",
                           channel, kIncomplete);
    case Health::WriteFailed:
        return std::format("{} audio storage failed while writing. {}", channel, kIncomplete);
    case Health::Crashed:
        return std::format("{} audio storage stopped unexpectedly. {}", channel, kIncomplete);
    case Health::RemainderFailed:
        return std::format("{} audio could not be finalized. {}", channel, kKept);
    case Health::SessionFinalizeFailed:
        return std::format("{} session audio could not be finalized. {}", channel, kKept);
    case Health::DidNotFinish:
        return std::format("{} audio storage did not finish. {}", channel, kKept);
    case Health::SessionFlush:
        return std::format("{} session audio could not be flushed. {}", channel, kKept);
    case Health::MetaWrite:
        return std::format("{} session metadata could not be written. {}", channel, kKept);
    case Health::SessionClose:
        return std::format("{} session audio could not be closed. {}", channel, kKept);
    case Health::Convert:
        return std::format("{} session audio could not be converted. {}", channel, kKept);
    case Health::SessionWrite:
        return std::format("{} session audio could not be written. {}", channel, kKept);
    case Health::ChunkWrite:
        return std::format("{} audio chunk could not be written. {}", channel, kKept);
    case Health::Register:
        return std::format(
            "{} audio could not be added to the meeting. Its file was kept for recovery.", channel);
    case Health::None:
        break;
    }
    return {};
}

std::filesystem::path with_suffix(const std::filesystem::path& path, std::string_view suffix) {
    std::filesystem::path out = path;
    out += std::filesystem::path(std::string(suffix));
    return out;
}

} // namespace

// --- pure helpers ------------------------------------------------------------------------------

std::vector<std::int16_t> resample_to_16k(std::span<const std::int16_t> frames, int src_rate) {
    if (frames.empty()) {
        return {};
    }
    if (src_rate == TARGET_RATE) {
        return {frames.begin(), frames.end()};
    }
    if (src_rate == 0) {
        throw py::PyError("ZeroDivisionError", "float division by zero");
    }
    std::vector<float> float_frames(frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        // numpy: float32(x) / 32768.0 stays in float32, and the division by a power of two is
        // exact.
        float_frames[i] = static_cast<float>(frames[i]) / 32768.0f;
    }
    const double exact =
        static_cast<double>(frames.size()) * TARGET_RATE / static_cast<double>(src_rate);
    const auto n_out =
        std::max<std::int64_t>(1, static_cast<std::int64_t>(py::round_half_even(exact)));
    const std::vector<float> resampled =
        dsp::fft_resample(float_frames, static_cast<std::size_t>(n_out));
    std::vector<std::int16_t> out(resampled.size());
    for (std::size_t i = 0; i < resampled.size(); ++i) {
        const float clipped = std::clamp(resampled[i], -1.0f, 1.0f);
        // float32 times the Python float 32767.0 is a float32 product; astype truncates.
        out[i] = static_cast<std::int16_t>(clipped * 32767.0f);
    }
    return out;
}

std::int64_t gap_fill_frame_count(std::optional<double> expected_start_s, double actual_start_s,
                                  int sample_rate, double tolerance_s) {
    if (!expected_start_s) {
        return 0;
    }
    const double gap_s = actual_start_s - *expected_start_s;
    if (gap_s <= tolerance_s) {
        return 0;
    }
    return std::max<std::int64_t>(0, static_cast<std::int64_t>(py::round_half_even(
                                         gap_s * static_cast<double>(sample_rate))));
}

std::vector<std::int16_t> gap_fill_frames(std::optional<double> expected_start_s,
                                          double actual_start_s, int sample_rate,
                                          double tolerance_s) {
    const std::int64_t count =
        gap_fill_frame_count(expected_start_s, actual_start_s, sample_rate, tolerance_s);
    return std::vector<std::int16_t>(static_cast<std::size_t>(std::max<std::int64_t>(0, count)));
}

std::vector<std::int64_t> gap_fill_pieces(std::int64_t frame_count, int sample_rate) {
    std::vector<std::int64_t> pieces;
    if (frame_count <= 0) {
        return pieces;
    }
    const auto piece = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(py::round_half_even(GAP_FILL_CHUNK_S * sample_rate)));
    for (std::int64_t remaining = frame_count; remaining > 0; remaining -= piece) {
        pieces.push_back(std::min(remaining, piece));
    }
    return pieces;
}

std::optional<std::int64_t> find_cut_point(std::span<const std::int16_t> buffer, int sample_rate,
                                           double target_sec, double max_sec, double quiet_rms,
                                           double quiet_window_s) {
    detail::CutScanner scanner(sample_rate, target_sec, max_sec, quiet_rms, quiet_window_s);
    scanner.push(buffer);
    return scanner.cut_index();
}

// --- SpoolWriter -------------------------------------------------------------------------------

struct SpoolWriter::Impl {
    Impl(SpoolWriter& owner, std::string meeting_id, const MeetingClock& clock,
         std::shared_ptr<meeting_state::MeetingRepository> repository,
         std::function<void(const meeting_state::SpooledChunk&)> on_chunk, SpoolOptions options)
        : owner(owner), meeting_id(std::move(meeting_id)), clock(clock),
          repository(std::move(repository)), on_chunk(std::move(on_chunk)),
          options(std::move(options)), ring(this->options.queue_size),
          seq(std::max<std::int64_t>(0, this->options.initial_seq)),
          session_16k_pcm(session_16k_pcm_path(owner.spool_dir_, owner.channel_)) {}

    SpoolWriter& owner;
    const std::string meeting_id;
    const MeetingClock& clock;
    const std::shared_ptr<meeting_state::MeetingRepository> repository;
    const std::function<void(const meeting_state::SpooledChunk&)> on_chunk;
    const SpoolOptions options;
    detail::BlockRing ring;
    std::atomic<bool> closed{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<int> health{0};
    std::mutex flush_mutex;
    std::mutex done_mutex;
    std::condition_variable done_cv;
    bool done = false;                                      // guarded by done_mutex
    std::optional<meeting_state::SpooledChunk> final_chunk; // guarded by done_mutex
    std::thread thread;

    // Writer-thread state.
    bool drop_logged = false;
    std::vector<std::int16_t> block; // the block being processed, copied out of the ring
    double block_t_meeting = 0.0;
    int block_rate = 0;
    std::optional<int> rate;
    std::vector<std::int16_t> pending; // native samples buffered for this chunk
    std::int64_t consumed = 0;         // native samples already cut into chunks
    double origin_s = 0.0;             // meeting time of the stream's first sample
    std::unique_ptr<detail::CutScanner> scanner;
    std::int64_t seq;
    detail::BinaryFile session_fp;
    std::int64_t session_samples = 0;
    std::optional<int> session_rate;
    double session_origin_s = 0.0;
    // The meeting time of the durable session's first sample: it survives rate changes and
    // source restarts, so the offline pass stays aligned.
    std::optional<double> session_meeting_origin_s;
    std::int64_t session_16k_samples = 0;
    std::int64_t session_meta_at = 0;
    const std::filesystem::path session_16k_pcm;

    const std::string& channel() const { return owner.channel_; }
    const std::filesystem::path& spool_dir() const { return owner.spool_dir_; }

    void record_health(Health value) noexcept {
        int expected = 0;
        health.compare_exchange_strong(expected, static_cast<int>(value));
    }

    void writer_loop();
    void log_drops();
    void process_block(std::span<const std::int16_t> frames, double t_meeting, int src_rate);
    void log_timeline_fix(bool fill, double seconds);
    double stream_end_s() const;
    void reset_stream(int new_rate, double new_origin_s);
    std::unique_ptr<detail::CutScanner> new_scanner(int scan_rate) const;
    void append_silence_frames(std::int64_t frame_count);
    void append(std::span<const std::int16_t> frames);
    void cut_ready_chunks();
    void finalize_remainder();
    void append_session(std::span<const std::int16_t> frames);
    void write_session_meta();
    std::optional<std::filesystem::path> close_session_pcm();
    void spill_session_pcm();
    void finalize_session();
    void emit_chunk(std::vector<std::int16_t> native, double start_s, int chunk_rate);
    std::optional<std::int64_t> register_chunk(const std::filesystem::path& file_path,
                                               std::int64_t chunk_seq, double start_s,
                                               double duration_s);
};

SpoolWriter::SpoolWriter(std::string meeting_id, std::string channel,
                         std::filesystem::path spool_dir, const MeetingClock& clock,
                         std::shared_ptr<meeting_state::MeetingRepository> repository,
                         std::function<void(const meeting_state::SpooledChunk&)> on_chunk,
                         SpoolOptions options)
    : channel_(std::move(channel)), spool_dir_(std::move(spool_dir)) {
    std::filesystem::create_directories(spool_dir_);
    impl_ = std::make_unique<Impl>(*this, std::move(meeting_id), clock, std::move(repository),
                                   std::move(on_chunk), std::move(options));
    impl_->thread = std::thread([impl = impl_.get()] { impl->writer_loop(); });
}

SpoolWriter::~SpoolWriter() {
    flush();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

void SpoolWriter::feed(const CaptureBlock& block) noexcept {
    Impl& impl = *impl_;
    if (impl.closed.load(std::memory_order_acquire)) {
        return;
    }
    if (impl.clock.is_paused()) {
        return;
    }
    if (block.frames == nullptr || block.count == 0) {
        return;
    }
    const double t_meeting = impl.clock.meeting_time(block.t_mono);
    if (!impl.ring.push(block.samples(), t_meeting, block.rate)) {
        dropped_.fetch_add(1);
        impl.record_health(Health::Dropped);
    }
}

std::optional<meeting_state::SpooledChunk> SpoolWriter::flush(double timeout_s) {
    Impl& impl = *impl_;
    const std::lock_guard flush_lock(impl.flush_mutex);
    if (!impl.closed.exchange(true)) {
        impl.stop_requested.store(true);
        std::unique_lock done_lock(impl.done_mutex);
        const bool finished = impl.done_cv.wait_for(
            done_lock, std::chrono::duration<double>(timeout_s), [&] { return impl.done; });
        done_lock.unlock();
        if (!finished) {
            impl.record_health(Health::DidNotFinish);
            logger.error("Spool writer for channel {} did not finish within {:.0f}s", channel_,
                         timeout_s);
        }
        if (const std::size_t dropped = dropped_.load(); dropped > 0) {
            logger.warning("Spool dropped {} capture block(s) on channel {}", dropped, channel_);
        }
    }
    const std::lock_guard done_lock(impl.done_mutex);
    return impl.final_chunk;
}

std::optional<std::string> SpoolWriter::health_error() const {
    const auto value = static_cast<Health>(impl_->health.load());
    if (value == Health::None) {
        return std::nullopt;
    }
    return health_sentence(value, channel_);
}

std::size_t SpoolWriter::queued_blocks() const {
    return impl_->ring.queued();
}

std::size_t SpoolWriter::queue_capacity() const {
    return impl_->ring.capacity();
}

void SpoolWriter::Impl::writer_loop() {
    detail::set_thread_name("fv-meeting-spool");
    try {
        while (true) {
            const bool got = ring.pop([&](const detail::QueuedBlock& queued) {
                block.assign(queued.samples.begin(), queued.samples.end());
                // Freed before processing, as queue.get() frees its slot.
                block_t_meeting = queued.t_meeting;
                block_rate = queued.rate;
            });
            log_drops();
            if (!got) {
                if (stop_requested.load()) {
                    break;
                }
                std::this_thread::sleep_for(kWriterPoll);
                continue;
            }
            if (options.before_block) {
                options.before_block();
            }
            try {
                process_block(block, block_t_meeting, block_rate);
            } catch (const std::exception& error) {
                record_health(Health::WriteFailed);
                logger.error_with(error,
                                  std::format("Spool writer failed on a block ({})", channel()));
            }
        }
    } catch (const std::exception& error) {
        record_health(Health::Crashed);
        logger.error_with(error, std::format("Spool writer thread crashed ({})", channel()));
    } catch (...) {
        record_health(Health::Crashed);
        logger.error("Spool writer thread crashed ({})", channel());
    }
    try {
        finalize_remainder();
    } catch (const std::exception& error) {
        record_health(Health::RemainderFailed);
        logger.error_with(error,
                          std::format("Spool remainder finalization failed ({})", channel()));
    }
    try {
        finalize_session();
    } catch (const std::exception& error) {
        record_health(Health::SessionFinalizeFailed);
        logger.error_with(error, std::format("Session WAV finalization failed ({})", channel()));
    }
    {
        const std::lock_guard lock(done_mutex);
        done = true;
    }
    done_cv.notify_all();
}

void SpoolWriter::Impl::log_drops() {
    // The device thread may not log, so the first drop is reported here.
    if (!drop_logged && owner.dropped_.load() > 0) {
        drop_logged = true;
        logger.error("Spool queue full on channel {}; dropping capture blocks (writer thread is "
                     "behind)",
                     channel());
    }
}

double SpoolWriter::Impl::stream_end_s() const {
    const int stream_rate = rate.value_or(TARGET_RATE);
    return origin_s + static_cast<double>(consumed + static_cast<std::int64_t>(pending.size())) /
                          static_cast<double>(stream_rate);
}

std::unique_ptr<detail::CutScanner> SpoolWriter::Impl::new_scanner(int scan_rate) const {
    return std::make_unique<detail::CutScanner>(scan_rate, options.target_sec, options.max_sec,
                                                QUIET_RMS, options.quiet_window_s);
}

void SpoolWriter::Impl::reset_stream(int new_rate, double new_origin_s) {
    rate = new_rate;
    origin_s = new_origin_s;
    consumed = 0;
    pending.clear();
    scanner = new_scanner(new_rate);
}

void SpoolWriter::Impl::process_block(std::span<const std::int16_t> frames, double t_meeting,
                                      int src_rate) {
    if (frames.empty()) {
        return;
    }
    const int incoming_rate = src_rate != 0 ? src_rate : TARGET_RATE;
    if (!rate) {
        reset_stream(incoming_rate, t_meeting);
        if (!session_meeting_origin_s) {
            session_meeting_origin_s = t_meeting;
        }
    } else if (incoming_rate != *rate) {
        logger.warning("Capture rate changed on channel {} ({} -> {} Hz); cutting the pending "
                       "chunk",
                       channel(), *rate, incoming_rate);
        finalize_remainder();
        // The meeting-time cursor crosses the rate boundary, so the next gap fill accounts for
        // the outage and the session keeps one origin.
        const double end_s = stream_end_s();
        spill_session_pcm();
        reset_stream(incoming_rate, end_s);
    }

    const double gap_s = t_meeting - stream_end_s();
    if (gap_s > GAP_TOLERANCE_S) {
        const std::int64_t fill =
            gap_fill_frame_count(stream_end_s(), t_meeting, rate.value_or(TARGET_RATE));
        if (fill > 0) {
            log_timeline_fix(true, gap_s);
            append_silence_frames(fill);
        }
    } else if (gap_s < -GAP_TOLERANCE_S) {
        // The stream already covers this span: a stalled source delivered buffered audio stamped
        // at delivery. Trim the overlap instead of pushing the channel ahead for good.
        const auto trim = std::min<std::int64_t>(
            static_cast<std::int64_t>(frames.size()),
            static_cast<std::int64_t>(py::round_half_even(-gap_s * static_cast<double>(*rate))));
        log_timeline_fix(false, -gap_s);
        frames = frames.subspan(static_cast<std::size_t>(std::max<std::int64_t>(0, trim)));
        if (frames.empty()) {
            return;
        }
    }
    append(frames);
    cut_ready_chunks();
}

void SpoolWriter::Impl::log_timeline_fix(bool fill, double seconds) {
    const int count = owner.gap_logs_.fetch_add(1) + 1;
    const std::string message =
        fill ? std::format("Filling {:.3f}s of silence on channel {}", seconds, channel())
             : std::format("Trimming {:.3f}s of overlap on channel {}", seconds, channel());
    if (count <= kMaxGapLogs) {
        logger.info("{}", message);
    } else {
        logger.debug("{}", message);
    }
}

void SpoolWriter::Impl::append_silence_frames(std::int64_t frame_count) {
    if (frame_count <= 0 || !rate) {
        return;
    }
    for (const std::int64_t n : gap_fill_pieces(frame_count, *rate)) {
        const std::vector<std::int16_t> zeros(static_cast<std::size_t>(n));
        append(zeros);
        cut_ready_chunks();
    }
}

void SpoolWriter::Impl::append(std::span<const std::int16_t> frames) {
    pending.insert(pending.end(), frames.begin(), frames.end());
    if (scanner) {
        scanner->push(frames);
    }
    append_session(frames);
}

void SpoolWriter::Impl::cut_ready_chunks() {
    while (scanner) {
        const std::optional<std::int64_t> cut = scanner->cut_index();
        if (!cut || *cut <= 0) {
            return;
        }
        const auto at = static_cast<std::size_t>(
            std::min<std::int64_t>(*cut, static_cast<std::int64_t>(pending.size())));
        std::vector<std::int16_t> chunk(pending.begin(),
                                        pending.begin() + static_cast<std::ptrdiff_t>(at));
        std::vector<std::int16_t> rest(pending.begin() + static_cast<std::ptrdiff_t>(at),
                                       pending.end());
        const int chunk_rate = rate.value_or(TARGET_RATE);
        const double start_s = origin_s + static_cast<double>(consumed) / chunk_rate;
        consumed += static_cast<std::int64_t>(chunk.size());
        pending = std::move(rest);
        scanner = new_scanner(chunk_rate);
        if (!pending.empty()) {
            scanner->push(pending);
        }
        emit_chunk(std::move(chunk), start_s, chunk_rate);
    }
}

void SpoolWriter::Impl::finalize_remainder() {
    if (pending.empty() || !rate) {
        pending.clear();
        return;
    }
    const int chunk_rate = *rate;
    std::vector<std::int16_t> buffered = std::move(pending);
    pending.clear();
    const double start_s = origin_s + static_cast<double>(consumed) / chunk_rate;
    consumed += static_cast<std::int64_t>(buffered.size());
    scanner = new_scanner(chunk_rate);
    const double seconds = static_cast<double>(buffered.size()) / chunk_rate;
    if (seconds < MIN_FLUSH_S) {
        logger.info("Dropping {:.2f}s sub-minimum spool remainder ({})", seconds, channel());
        return;
    }
    emit_chunk(std::move(buffered), start_s, chunk_rate);
}

void SpoolWriter::Impl::append_session(std::span<const std::int16_t> frames) {
    if (frames.empty() || !rate) {
        return;
    }
    const bool opened = !session_fp.is_open();
    if (opened) {
        session_fp = detail::BinaryFile::open(session_pcm_path(spool_dir(), channel()), "wb");
        session_rate = *rate;
        if (!session_meeting_origin_s) {
            session_meeting_origin_s = origin_s;
        }
        // The immutable first-sample meeting time is what the meta always publishes.
        session_origin_s = *session_meeting_origin_s;
        session_samples = 0;
        session_meta_at = 0;
    }
    session_fp.write(std::as_bytes(frames));
    session_samples += static_cast<std::int64_t>(frames.size());
    // The session PCM is at the device rate; the interval is in seconds.
    const auto meta_every =
        static_cast<std::int64_t>(kSessionMetaEveryS * session_rate.value_or(TARGET_RATE));
    if (opened || session_samples - session_meta_at >= meta_every) {
        write_session_meta();
        session_meta_at = session_samples;
    }
}

void SpoolWriter::Impl::write_session_meta() {
    if (session_fp.is_open()) {
        try {
            session_fp.flush();
        } catch (const std::system_error& error) {
            record_health(Health::SessionFlush);
            logger.error_with(error, std::format("Session PCM flush failed ({})", channel()));
        }
    }
    const double origin = session_meeting_origin_s.value_or(session_origin_s);
    int meta_rate = 0;
    std::int64_t samples = 0;
    double pcm_origin = 0.0;
    bool pcm_active = true;
    // Once a segment has spilled, the durable 16 kHz total places the native suffix, so offline
    // readers see one timeline origin even mid-meeting.
    if (session_rate && session_samples > 0) {
        meta_rate = *session_rate;
        samples = session_samples;
        pcm_origin = origin + static_cast<double>(session_16k_samples) / TARGET_RATE;
        pcm_active = true;
    } else if (session_16k_samples > 0) {
        meta_rate = TARGET_RATE;
        samples = session_16k_samples;
        pcm_origin = origin;
        pcm_active = false;
    } else {
        return;
    }
    try {
        meeting::write_session_meta(session_meta_path(spool_dir(), channel()), meta_rate, samples,
                                    origin, pcm_origin, session_16k_samples, pcm_active);
    } catch (const std::exception& error) {
        record_health(Health::MetaWrite);
        logger.error_with(error, std::format("Session metadata write failed ({})", channel()));
    }
}

std::optional<std::filesystem::path> SpoolWriter::Impl::close_session_pcm() {
    if (session_fp.is_open()) {
        detail::BinaryFile handle = std::move(session_fp);
        try {
            handle.flush();
            handle.close();
        } catch (const std::system_error& error) {
            record_health(Health::SessionClose);
            logger.error_with(error, std::format("Session PCM close failed ({})", channel()));
        }
    }
    write_session_meta();
    const std::filesystem::path pcm_path = session_pcm_path(spool_dir(), channel());
    std::error_code error;
    if (session_samples <= 0 || !std::filesystem::is_regular_file(pcm_path, error)) {
        return std::nullopt;
    }
    return pcm_path;
}

void SpoolWriter::Impl::spill_session_pcm() {
    const std::optional<std::filesystem::path> pcm_path = close_session_pcm();
    const std::optional<int> pcm_rate = session_rate;
    if (!pcm_path || !pcm_rate) {
        session_samples = 0;
        session_rate.reset();
        return;
    }
    bool converted = false;
    try {
        std::error_code ignored;
        const std::int64_t written =
            resample_pcm_file_to_16k(*pcm_path, *pcm_rate, session_16k_pcm,
                                     std::filesystem::is_regular_file(session_16k_pcm, ignored));
        session_16k_samples += written;
        converted = true;
    } catch (const std::exception& error) {
        record_health(Health::Convert);
        logger.error_with(error, std::format("Session PCM resample failed ({})", channel()));
    }
    if (converted) {
        std::error_code error;
        if (!std::filesystem::remove(*pcm_path, error) && error) {
            logger.debug("Could not remove converted session PCM {}", path_to_utf8(*pcm_path));
        }
    }
    session_samples = 0;
    session_rate.reset();
    // The meta now describes the durable 16 kHz timeline.
    write_session_meta();
}

void SpoolWriter::Impl::finalize_session() {
    spill_session_pcm();
    std::error_code error;
    if (!std::filesystem::is_regular_file(session_16k_pcm, error)) {
        return;
    }
    try {
        pcm16k_to_wav(session_16k_pcm, session_wav_path(spool_dir(), channel()));
    } catch (const std::exception& failure) {
        record_health(Health::SessionWrite);
        logger.error_with(failure, std::format("Session WAV wrap failed ({})", channel()));
        return;
    }
    if (session_16k_samples <= 0) {
        const auto size = std::filesystem::file_size(session_16k_pcm, error);
        if (!error) {
            session_16k_samples = static_cast<std::int64_t>(size / 2);
        }
    }
    // The final meta describes the 16 kHz session WAV.
    session_rate = TARGET_RATE;
    session_samples = session_16k_samples;
    write_session_meta();
    std::filesystem::remove(session_16k_pcm, error);
}

void SpoolWriter::Impl::emit_chunk(std::vector<std::int16_t> native, double start_s,
                                   int chunk_rate) {
    // The order is pinned: the WAV lands on disk, the row lands as pending, and only then does
    // on_chunk run, so a crash at any point leaves work recovery can find. Failures are logged
    // and swallowed; the stream keeps running.
    const std::vector<std::int16_t> samples = resample_to_16k(native, chunk_rate);
    if (samples.empty()) {
        return;
    }
    const std::int64_t chunk_seq = seq++;
    const double duration_s = static_cast<double>(samples.size()) / TARGET_RATE;
    const std::filesystem::path file_path =
        spool_dir() / path_from_utf8(std::format("{}_{:05d}.wav", channel(), chunk_seq));
    const std::filesystem::path tmp_path = with_suffix(file_path, ".tmp");
    try {
        {
            detail::WavOut wav(tmp_path, TARGET_RATE);
            wav.write(std::span<const std::int16_t>(samples));
            wav.close();
        }
        detail::replace_file(tmp_path, file_path);
    } catch (const std::exception& error) {
        record_health(Health::ChunkWrite);
        logger.error_with(error,
                          std::format("Failed to write spool chunk {}", path_to_utf8(file_path)));
        std::error_code ignored;
        std::filesystem::remove(tmp_path, ignored);
        return;
    }

    // The registration marker goes out before the database is touched: a hard kill after the
    // WAV's replace and before or during the commit is reconciled at startup; a registration
    // removes it.
    try {
        write_chunk_recovery_meta(file_path, meeting_id, channel(), chunk_seq, start_s, duration_s,
                                  TARGET_RATE);
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Failed to publish recovery metadata for {}",
                                             path_to_utf8(file_path)));
    }

    const std::optional<std::int64_t> chunk_id =
        register_chunk(file_path, chunk_seq, start_s, duration_s);
    if (!chunk_id) {
        return;
    }
    const meeting_state::SpooledChunk chunk{
        .chunk_id = *chunk_id,
        .meeting_id = meeting_id,
        .channel = channel(),
        .seq = chunk_seq,
        .file_path = path_to_utf8(file_path),
        .start_s = start_s,
        .duration_s = duration_s,
        .sample_rate = TARGET_RATE,
    };
    {
        const std::lock_guard lock(done_mutex);
        final_chunk = chunk;
    }
    logger.debug("Spooled chunk {}: {:.2f}s at {:.2f}s ({})", *chunk_id, duration_s, start_s,
                 channel());
    if (on_chunk) {
        try {
            on_chunk(chunk);
        } catch (const std::exception& error) {
            logger.error_with(error,
                              std::format("on_chunk callback raised for chunk {}", *chunk_id));
        } catch (...) {
            // The engine's callback; nothing it throws may end the writer thread.
            logger.error("on_chunk callback raised for chunk {}", *chunk_id);
        }
    }
}

std::optional<std::int64_t>
SpoolWriter::Impl::register_chunk(const std::filesystem::path& file_path, std::int64_t chunk_seq,
                                  double start_s, double duration_s) {
    const std::string path_text = path_to_utf8(file_path);
    for (int attempt = 1; attempt <= 2; ++attempt) {
        try {
            Json fields = Json::object();
            fields["meeting_id"] = meeting_id;
            fields["channel"] = channel();
            fields["seq"] = chunk_seq;
            fields["file_path"] = path_text;
            fields["start_s"] = start_s;
            fields["duration_s"] = duration_s;
            fields["sample_rate"] = TARGET_RATE;
            fields["asr_status"] = "pending";
            const std::int64_t chunk_id = repository->register_chunk(fields);
            remove_chunk_recovery_meta(file_path);
            return chunk_id;
        } catch (const std::exception& error) {
            logger.error_with(error, std::format("Failed to register spool chunk {} (attempt {})",
                                                 path_text, attempt));
            if (attempt == 1) {
                std::this_thread::sleep_for(kRegisterRetryDelay);
            }
        }
    }
    std::filesystem::path orphan_path = with_suffix(file_path, ".orphan");
    try {
        detail::replace_file(file_path, orphan_path);
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Failed to rename orphaned chunk {}", path_text));
        orphan_path = file_path;
    }
    try {
        write_chunk_recovery_meta(orphan_path, meeting_id, channel(), chunk_seq, start_s,
                                  duration_s, TARGET_RATE);
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Failed to preserve recovery metadata for {}",
                                             path_to_utf8(orphan_path)));
    }
    logger.error("Chunk {} could not be registered; audio kept at {} for startup recovery",
                 path_text, path_to_utf8(orphan_path));
    record_health(Health::Register);
    return std::nullopt;
}

} // namespace fv::meeting
