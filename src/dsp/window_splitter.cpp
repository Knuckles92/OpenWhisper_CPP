#include "dsp/window_splitter.h"

#include <cmath>
#include <cstddef>
#include <utility>

namespace fv::dsp {

namespace {

// numpy's pairwise summation of a float32 run (FLOAT_pairwise_sum): eight accumulators over
// blocks of at most 128, halves above that, all in float. Matching its order keeps the block
// energies, and so the argmin and its tie-breaking, bit-identical.
float pairwise_sum(const float* values, std::size_t count) {
    constexpr std::size_t block = 128;
    if (count < 8) {
        float sum = 0.0f;
        for (std::size_t i = 0; i < count; ++i) {
            sum += values[i];
        }
        return sum;
    }
    if (count <= block) {
        float r[8];
        for (std::size_t j = 0; j < 8; ++j) {
            r[j] = values[j];
        }
        std::size_t i = 8;
        for (; i < count - (count % 8); i += 8) {
            for (std::size_t j = 0; j < 8; ++j) {
                r[j] += values[i + j];
            }
        }
        float sum = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < count; ++i) {
            sum += values[i];
        }
        return sum;
    }
    std::size_t half = count / 2;
    half -= half % 8;
    return pairwise_sum(values, half) + pairwise_sum(values + half, count - half);
}

} // namespace

std::size_t split_point(std::span<const float> audio) {
    if (audio.size() < static_cast<std::size_t>(MAX_SAMPLES)) {
        return audio.size();
    }
    constexpr auto block = static_cast<std::size_t>(SPLIT_BLOCK_SAMPLES);
    constexpr auto blocks = static_cast<std::size_t>((MAX_SAMPLES - MIN_SAMPLES) / block);
    float squares[block];
    std::size_t quietest = 0;
    float lowest = 0.0f;
    for (std::size_t b = 0; b < blocks; ++b) {
        const float* samples = audio.data() + MIN_SAMPLES + b * block;
        for (std::size_t i = 0; i < block; ++i) {
            squares[i] = samples[i] * samples[i];
        }
        // np.mean: the float32 sum over the count, rounded to float32. Dividing in double first
        // gives the same float, since double has more than twice float's precision.
        const auto energy = static_cast<float>(static_cast<double>(pairwise_sum(squares, block)) /
                                               static_cast<double>(block));
        // np.argmin: the first minimum, and the first NaN wins over any number.
        const bool take =
            b == 0 || (!std::isnan(lowest) && (std::isnan(energy) || energy < lowest));
        if (take) {
            quietest = b;
            lowest = energy;
        }
    }
    return static_cast<std::size_t>(MIN_SAMPLES) + (quietest + 1) * block;
}

std::vector<Window> WindowSplitter::push(std::span<const std::int16_t> s16_16k) {
    // No reserve: an exact reserve per small frame would reallocate on every push.
    for (const std::int16_t sample : s16_16k) {
        pending_.push_back(static_cast<float>(sample) / 32768.0f);
    }
    std::vector<Window> ready;
    std::size_t consumed = 0;
    while (pending_.size() - consumed >= static_cast<std::size_t>(MAX_SAMPLES)) {
        const std::span<const float> rest = std::span<const float>(pending_).subspan(consumed);
        const std::size_t end = split_point(rest);
        ready.push_back(Window{static_cast<double>(offset_) / static_cast<double>(SAMPLE_RATE),
                               std::vector<float>(rest.begin(), rest.begin() + end)});
        offset_ += static_cast<std::int64_t>(end);
        consumed += end;
    }
    if (consumed > 0) {
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
    return ready;
}

std::vector<Window> WindowSplitter::finish() {
    if (pending_.empty()) {
        return {};
    }
    std::vector<Window> last;
    last.push_back(Window{static_cast<double>(offset_) / static_cast<double>(SAMPLE_RATE),
                          std::move(pending_)});
    offset_ += static_cast<std::int64_t>(last.front().samples.size());
    pending_ = {};
    return last;
}

std::int64_t WindowSplitter::total() const {
    return offset_ + static_cast<std::int64_t>(pending_.size());
}

std::int64_t WindowSplitter::offset() const {
    return offset_;
}

} // namespace fv::dsp
