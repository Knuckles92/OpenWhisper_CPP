#include "dsp/level.h"

#include <cmath>
#include <cstddef>

namespace fv::dsp {

namespace {

double level_from_mean_square(double mean_square, double full_scale) {
    constexpr auto floor_db = static_cast<double>(LEVEL_HISTOGRAM_MIN_DB);
    // `!(x > 0)` also sends NaN to the floor, as Python's max(floor, nan) keeps the floor.
    if (!(mean_square > 0.0)) {
        return floor_db;
    }
    const double level = 10.0 * std::log10(mean_square) - 20.0 * std::log10(full_scale);
    return level > floor_db ? level : floor_db;
}

} // namespace

double block_level_db(std::span<const std::int16_t> samples) {
    if (samples.empty()) {
        return static_cast<double>(LEVEL_HISTOGRAM_MIN_DB);
    }
    // Each square is at most 2^30, so an integer sum is exact; numpy's float64 dot is exact too
    // up to 2^23 samples, so both give the same mean square.
    std::uint64_t sum = 0;
    for (const std::int16_t sample : samples) {
        const auto value = static_cast<std::int64_t>(sample);
        sum += static_cast<std::uint64_t>(value * value);
    }
    const double mean_square = static_cast<double>(sum) / static_cast<double>(samples.size());
    return level_from_mean_square(mean_square, 32768.0);
}

double block_level_db(std::span<const float> samples) {
    if (samples.empty()) {
        return static_cast<double>(LEVEL_HISTOGRAM_MIN_DB);
    }
    double sum = 0.0;
    for (const float sample : samples) {
        const auto value = static_cast<double>(sample);
        sum += value * value;
    }
    return level_from_mean_square(sum / static_cast<double>(samples.size()), 1.0);
}

} // namespace fv::dsp
