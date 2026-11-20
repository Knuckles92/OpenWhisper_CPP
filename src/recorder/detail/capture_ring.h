#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The lock-free hand-over from a device callback to the recorder's audio-io thread: a
// single-producer, single-consumer ring of preallocated blocks. The producer side touches only
// preallocated memory and atomics (architecture section 4, rule 4).
namespace fv::recorder_detail {

struct BlockFlags {
    bool overflow = false;
    bool underflow = false;
    // Captured while the recording was paused: kept only for its status.
    bool paused = false;
};

// Affinity: push() on one producer thread, drain() and empty() on one consumer thread.
class CaptureRing {
public:
    // `slots` blocks of up to `slot_samples` interleaved samples each.
    CaptureRing(std::size_t slots, std::size_t slot_samples)
        : slot_samples_(std::max<std::size_t>(slot_samples, 1)), samples_(slots * slot_samples_),
          meta_(slots) {}
    CaptureRing(const CaptureRing&) = delete;
    CaptureRing& operator=(const CaptureRing&) = delete;

    std::size_t slot_samples() const { return slot_samples_; }

    // One block of `frames` frames (`samples` interleaved samples, at most slot_samples()).
    // False when the ring is full: the consumer has fallen 512 blocks behind.
    bool push(const std::int16_t* samples, std::size_t sample_count, std::size_t frames,
              BlockFlags flags) noexcept {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= meta_.size()) {
            return false;
        }
        const std::size_t slot = static_cast<std::size_t>(head % meta_.size());
        std::copy_n(samples, sample_count, samples_.data() + slot * slot_samples_);
        meta_[slot] = Meta{sample_count, frames, flags};
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Hands every queued block to fn(span<const int16_t> samples, size_t frames, BlockFlags) in
    // order; returns how many.
    template <class Fn> std::size_t drain(Fn&& fn) {
        std::size_t count = 0;
        std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        for (; tail != head; ++tail, ++count) {
            const std::size_t slot = static_cast<std::size_t>(tail % meta_.size());
            const Meta& meta = meta_[slot];
            fn(std::span<const std::int16_t>(samples_.data() + slot * slot_samples_,
                                             meta.sample_count),
               meta.frames, meta.flags);
            tail_.store(tail + 1, std::memory_order_release);
        }
        return count;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

private:
    struct Meta {
        std::size_t sample_count = 0;
        std::size_t frames = 0;
        BlockFlags flags;
    };

    std::size_t slot_samples_;
    std::vector<std::int16_t> samples_;
    std::vector<Meta> meta_;
    std::atomic<std::uint64_t> head_{0}; // written by the producer
    std::atomic<std::uint64_t> tail_{0}; // written by the consumer
};

} // namespace fv::recorder_detail
