#include "audio/pcm_tape.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace fv::audio {

std::int64_t TapeView::first() const {
    return first_;
}

std::int64_t TapeView::last() const {
    return last_;
}

std::int64_t TapeView::frames() const {
    return last_ - first_;
}

int TapeView::rate() const {
    return rate_;
}

int TapeView::channels() const {
    return channels_;
}

void TapeView::copy_to(std::span<std::int16_t> out) const {
    const auto wanted = static_cast<std::size_t>(frames() * channels_);
    if (out.size() != wanted) {
        throw std::invalid_argument("TapeView::copy_to needs room for exactly " +
                                    std::to_string(wanted) + " samples");
    }
    std::size_t written = 0;
    std::int64_t frame = first_;
    while (frame < last_) {
        const std::int64_t relative = frame - chunk_origin_;
        const auto chunk = static_cast<std::size_t>(relative / chunk_frames_);
        const std::int64_t offset = relative % chunk_frames_;
        const std::int64_t take = std::min(chunk_frames_ - offset, last_ - frame);
        const std::int16_t* source = (*chunks_)[chunk].get() + offset * channels_;
        std::copy_n(source, take * channels_, out.data() + written);
        written += static_cast<std::size_t>(take * channels_);
        frame += take;
    }
}

PcmTape::PcmTape(int rate, int channels) : rate_(rate), channels_(channels), chunk_frames_(rate) {
    if (rate <= 0 || channels <= 0) {
        throw std::invalid_argument("PcmTape needs a positive rate and channel count");
    }
}

void PcmTape::append(std::span<const std::int16_t> samples) {
    if (samples.size() % static_cast<std::size_t>(channels_) != 0) {
        throw std::invalid_argument("PcmTape::append takes whole frames");
    }
    std::int64_t frames = frames_.load(std::memory_order_relaxed);
    std::size_t done = 0;
    while (done < samples.size()) {
        const std::int64_t offset = frames % chunk_frames_;
        if (offset == 0 && frames / chunk_frames_ == static_cast<std::int64_t>(chunks_.size())) {
            // Allocated outside the lock: readers only ever wait for the list copy.
            detail::TapeChunk chunk(new std::int16_t[static_cast<std::size_t>(chunk_frames_) *
                                                     static_cast<std::size_t>(channels_)]);
            const std::lock_guard lock(mutex_);
            chunks_.push_back(std::move(chunk));
        }
        const auto remaining_frames = static_cast<std::int64_t>(
            (samples.size() - done) / static_cast<std::size_t>(channels_));
        const std::int64_t take = std::min(chunk_frames_ - offset, remaining_frames);
        std::int16_t* target =
            chunks_[static_cast<std::size_t>(frames / chunk_frames_)].get() + offset * channels_;
        std::copy_n(samples.data() + done, take * channels_, target);
        done += static_cast<std::size_t>(take * channels_);
        frames += take;
        // Publishes the samples just written to readers that load frames_ with acquire.
        frames_.store(frames, std::memory_order_release);
    }
}

TapeView PcmTape::view(std::int64_t first, std::int64_t last) const {
    const std::int64_t available = frames_.load(std::memory_order_acquire);
    if (first < 0 || last < first || last > available) {
        throw std::out_of_range("PcmTape::view [" + std::to_string(first) + ", " +
                                std::to_string(last) + ") is outside [0, " +
                                std::to_string(available) + ")");
    }
    TapeView view;
    view.first_ = first;
    view.last_ = last;
    view.rate_ = rate_;
    view.channels_ = channels_;
    view.chunk_frames_ = chunk_frames_;
    const std::int64_t first_chunk = first / chunk_frames_;
    view.chunk_origin_ = first_chunk * chunk_frames_;
    auto chunks = std::make_shared<std::vector<detail::TapeChunk>>();
    if (last > first) {
        const std::int64_t last_chunk = (last - 1) / chunk_frames_;
        const std::lock_guard lock(mutex_);
        chunks->assign(chunks_.begin() + first_chunk, chunks_.begin() + last_chunk + 1);
    }
    view.chunks_ = std::move(chunks);
    return view;
}

std::int64_t PcmTape::frames() const {
    return frames_.load(std::memory_order_acquire);
}

int PcmTape::rate() const {
    return rate_;
}

int PcmTape::channels() const {
    return channels_;
}

} // namespace fv::audio
