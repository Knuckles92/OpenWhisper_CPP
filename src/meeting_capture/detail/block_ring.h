#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The spool's bounded queue between a device callback and its writer thread (spool.py's
// queue.Queue(maxsize=512)): a single-producer, single-consumer ring of preallocated slots. The
// producer touches only preallocated memory and atomics, so feed() never blocks or allocates.
namespace fv::meeting::detail {

struct QueuedBlock {
    std::span<const std::int16_t> samples;
    double t_meeting = 0.0;
    int rate = 0;
};

// Affinity: push() on one producer thread at a time, pop() on one consumer thread.
class BlockRing {
public:
    // Samples one slot holds: four of PortAudio's 1024-frame blocks. A wider block takes several
    // consecutive slots, each stamped where its first sample falls.
    static constexpr std::size_t kSlotSamples = 4096;

    explicit BlockRing(std::size_t slots)
        : slots_(std::max<std::size_t>(slots, 1)), samples_(slots_ * kSlotSamples), meta_(slots_) {}
    BlockRing(const BlockRing&) = delete;
    BlockRing& operator=(const BlockRing&) = delete;

    std::size_t capacity() const { return slots_; }
    std::size_t queued() const {
        return static_cast<std::size_t>(head_.load(std::memory_order_acquire) -
                                        tail_.load(std::memory_order_acquire));
    }

    // The whole block or nothing; false when the slots it needs are not free.
    bool push(std::span<const std::int16_t> samples, double t_meeting, int rate) noexcept {
        const std::size_t needed = (samples.size() + kSlotSamples - 1) / kSlotSamples;
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail + needed > slots_) {
            return false;
        }
        for (std::size_t piece = 0; piece < needed; ++piece) {
            const std::size_t at = piece * kSlotSamples;
            const std::size_t count = std::min(kSlotSamples, samples.size() - at);
            const auto slot = static_cast<std::size_t>((head + piece) % slots_);
            std::copy_n(samples.data() + at, count, samples_.data() + slot * kSlotSamples);
            const double offset = rate > 0 ? static_cast<double>(at) / rate : 0.0;
            meta_[slot] = Meta{count, t_meeting + offset, rate};
        }
        head_.store(head + needed, std::memory_order_release);
        return true;
    }

    // Hands the oldest block to fn(const QueuedBlock&) and frees its slot; false when empty.
    template <class Fn> bool pop(Fn&& fn) {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            return false;
        }
        const auto slot = static_cast<std::size_t>(tail % slots_);
        const Meta& meta = meta_[slot];
        fn(QueuedBlock{
            std::span<const std::int16_t>(samples_.data() + slot * kSlotSamples, meta.count),
            meta.t_meeting, meta.rate});
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

private:
    struct Meta {
        std::size_t count = 0;
        double t_meeting = 0.0;
        int rate = 0;
    };

    std::size_t slots_;
    std::vector<std::int16_t> samples_;
    std::vector<Meta> meta_;
    std::atomic<std::uint64_t> head_{0}; // written by the producer
    std::atomic<std::uint64_t> tail_{0}; // written by the consumer
};

} // namespace fv::meeting::detail
