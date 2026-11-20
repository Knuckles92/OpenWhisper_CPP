#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace fv::audio {

namespace detail {
// One second of interleaved samples. A chunk is written only past the frames a reader may see,
// so readers and the single writer never touch the same sample.
using TapeChunk = std::shared_ptr<std::int16_t[]>;
} // namespace detail

// Frames [first, last) of a PcmTape. Shares the tape's chunks (no copy); stays valid after the
// tape grows or is destroyed.
// Affinity: thread-safe (immutable).
class TapeView {
public:
    TapeView() = default;

    std::int64_t first() const;
    std::int64_t last() const;
    std::int64_t frames() const; // last() - first()
    int rate() const;
    int channels() const;
    void copy_to(std::span<std::int16_t> out) const; // out.size() == frames() * channels()

private:
    friend class PcmTape;

    std::shared_ptr<const std::vector<detail::TapeChunk>> chunks_; // from the chunk of first()
    std::int64_t chunk_origin_ = 0;                                // frame where chunks_[0] starts
    std::int64_t first_ = 0;
    std::int64_t last_ = 0;
    std::int64_t chunk_frames_ = 0;
    int rate_ = 0;
    int channels_ = 1;
};

// Append-only int16 store in 1 s chunks, one writer.
// Affinity: append on the audio-io thread only; everything else thread-safe.
class PcmTape {
public:
    // rate in Hz; channels interleaved.
    explicit PcmTape(int rate, int channels = 1);
    PcmTape(const PcmTape&) = delete;
    PcmTape& operator=(const PcmTape&) = delete;

    // Interleaved samples, whole frames only (std::invalid_argument otherwise).
    void append(std::span<const std::int16_t>); // audio-io thread only
    // [first, last) within [0, frames()]; std::out_of_range otherwise.
    TapeView view(std::int64_t first, std::int64_t last) const; // zero-copy, shares chunks
    std::int64_t frames() const;                                // atomic
    int rate() const;
    int channels() const;

private:
    int rate_;
    int channels_;
    std::int64_t chunk_frames_;
    mutable std::mutex mutex_;              // guards chunks_; held only to copy or extend the list
    std::vector<detail::TapeChunk> chunks_; // the writer's list
    std::atomic<std::int64_t> frames_{0};
};

} // namespace fv::audio
