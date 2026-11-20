#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "meeting_capture/capture_block.h"
#include "meeting_capture/meeting_clock.h"
#include "meeting_state/repository.h"
#include "meeting_state/types.h"

// meeting/capture/spool.py: the durable chunk spool. One SpoolWriter per channel turns capture
// blocks into a continuous 16 kHz mono int16 stream on the meeting clock (silence fills a source
// that went quiet), cuts chunks at quiet points after a target length (a hard cut at a maximum),
// writes each chunk atomically, registers it as pending, and only then hands it on: a chunk on
// disk and in the database survives a hard kill and is found by recovery.
//
// Audio is buffered at the device's native rate and resampled to 16 kHz once per chunk:
// resampling every block rounds each to whole output samples 43 times a second (371.52 -> 372 at
// 44.1 kHz), which drifts about 4.7 s per hour and makes spurious gap fills, and joins
// independently resampled blocks with an artifact at every boundary. Chunk offsets come from
// native sample counts, so consecutive chunks are contiguous by construction.
namespace fv::meeting {

inline constexpr int TARGET_RATE = 16000;
// Gaps longer than this (meeting seconds) are filled with silence; overlaps longer than this are
// trimmed off the incoming block.
inline constexpr double GAP_TOLERANCE_S = 0.12;
// RMS (int16 units) below which audio is quiet enough to cut at, and how long it must last.
inline constexpr double QUIET_RMS = 300.0;
inline constexpr double QUIET_WINDOW_S = 0.4;
// flush() drops a remainder shorter than this, so a meeting's last words survive and only
// slivers go.
inline constexpr double MIN_FLUSH_S = 0.25;
// Capture blocks the queue holds before dropping (about 12 s at 1024 frames and 44.1 kHz, far
// beyond the worst SQLite busy-wait measured).
inline constexpr std::size_t QUEUE_MAX_BLOCKS = 512;
// The preferred chunk length before quiet-cut scanning (short, so the dashboard sees near-live
// transcripts) and the hard maximum when a speaker never pauses.
inline constexpr double DEFAULT_TARGET_SEC = 5.0;
inline constexpr double DEFAULT_MAX_SEC = 20.0;
inline constexpr double FLUSH_TIMEOUT_S = 30.0;
// Silence for a gap is written in pieces of at most this, so a long suspend allocates little.
inline constexpr double GAP_FILL_CHUNK_S = 1.0;
// Native session PCM is resampled to 16 kHz in windows of this length: long enough to avoid the
// per-block artifact, short enough to bound memory on a long meeting.
inline constexpr double SESSION_RESAMPLE_WINDOW_S = 30.0;

// Mono int16 at `src_rate` to 16 kHz: numpy's float32 FFT resampling (dsp::fft_resample), clipped
// to [-1, 1], scaled by 32767 and truncated. The input itself at 16 kHz. Call it once per chunk,
// never per block: the FFT treats its input as one period of a periodic signal (a 1 kHz tone at
// 48 kHz measured THD+N -87 dB whole and -29 dB block-wise). A rate of 0 throws
// py::PyError("ZeroDivisionError") as Python's division does.
std::vector<std::int16_t> resample_to_16k(std::span<const std::int16_t> frames, int src_rate);

// Silence frames that bridge a capture gap: none for the first block (no expected start) or a gap
// within the tolerance, else round(gap * rate).
std::int64_t gap_fill_frame_count(std::optional<double> expected_start_s, double actual_start_s,
                                  int sample_rate = TARGET_RATE,
                                  double tolerance_s = GAP_TOLERANCE_S);
std::vector<std::int16_t> gap_fill_frames(std::optional<double> expected_start_s,
                                          double actual_start_s, int sample_rate = TARGET_RATE,
                                          double tolerance_s = GAP_TOLERANCE_S);
// The pieces SpoolWriter appends a gap's silence in: at most GAP_FILL_CHUNK_S each, so a
// multi-hour suspend never allocates the whole interval at once.
std::vector<std::int64_t> gap_fill_pieces(std::int64_t frame_count, int sample_rate);

// Where to cut the next chunk of a pending buffer: after target_sec, the end of the first window
// of quiet_window_s whose RMS stays below quiet_rms (the chunk keeps its quiet tail and the next
// starts past the silence), else max_sec; nullopt when no cut is due. The one-shot form of what
// SpoolWriter runs incrementally.
std::optional<std::int64_t> find_cut_point(std::span<const std::int16_t> buffer, int sample_rate,
                                           double target_sec, double max_sec,
                                           double quiet_rms = QUIET_RMS,
                                           double quiet_window_s = QUIET_WINDOW_S);

struct SpoolOptions {
    double target_sec = DEFAULT_TARGET_SEC;
    double max_sec = DEFAULT_MAX_SEC;
    double quiet_window_s = QUIET_WINDOW_S;
    std::size_t queue_size = QUEUE_MAX_BLOCKS;
    // The first per-channel sequence number, after a restart, so files and rows never collide.
    std::int64_t initial_seq = 0;
    // Tests only: runs on the writer thread before each queued block is processed.
    std::function<void()> before_block;
};

// Affinity: feed() on the channel's device thread (one producer at a time; it never blocks,
// allocates or logs); flush() and the queries on any thread. A writer thread "fv-meeting-spool"
// does the buffering, cutting, resampling, writing, registration and on_chunk, in that order.
class SpoolWriter {
public:
    // Creates spool_dir. on_chunk runs on the writer thread with each registered chunk.
    SpoolWriter(std::string meeting_id, std::string channel, std::filesystem::path spool_dir,
                const MeetingClock& clock,
                std::shared_ptr<meeting_state::MeetingRepository> repository,
                std::function<void(const meeting_state::SpooledChunk&)> on_chunk,
                SpoolOptions options = {});
    // Flushes (and waits for the writer thread) when flush() never ran.
    ~SpoolWriter();
    SpoolWriter(const SpoolWriter&) = delete;
    SpoolWriter& operator=(const SpoolWriter&) = delete;

    // Queues the block's native-rate samples with its meeting time. Blocks fed while the clock is
    // paused or after flush() are dropped (meeting time stands still, so no gap results); a full
    // queue drops the block and marks the audio incomplete.
    void feed(const CaptureBlock& block) noexcept;
    // Stops the writer, finalizes the remainder (one shorter than MIN_FLUSH_S is dropped) and the
    // session WAV, waiting at most timeout_s. Safe to call again; later calls return the same
    // chunk. Returns the last chunk the writer registered.
    std::optional<meeting_state::SpooledChunk> flush(double timeout_s = FLUSH_TIMEOUT_S);

    // The first storage failure, as a sentence for the meeting's status; nullopt while healthy.
    std::optional<std::string> health_error() const;
    std::size_t dropped_blocks() const { return dropped_.load(); }
    std::size_t queued_blocks() const;
    std::size_t queue_capacity() const;
    // Gap fills and overlap trims so far (the first five are logged at INFO).
    int timeline_fixes() const { return gap_logs_.load(); }
    const std::string& channel() const { return channel_; }
    const std::filesystem::path& spool_dir() const { return spool_dir_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string channel_;
    std::filesystem::path spool_dir_;
    std::atomic<std::size_t> dropped_{0};
    std::atomic<int> gap_logs_{0};
};

} // namespace fv::meeting
