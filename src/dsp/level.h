#pragma once

#include <cstdint>
#include <span>

namespace fv::dsp {

// The floor of the post-roll level histogram, in dBFS: a block is never reported quieter.
inline constexpr int LEVEL_HISTOGRAM_MIN_DB = -120;

// RMS level of one block in dBFS, floored at LEVEL_HISTOGRAM_MIN_DB; an empty or all-zero block
// is the floor. int16 full scale is 32768, float full scale is 1.0. The mean square is taken in
// double, as numpy's dot product of the block cast to float64.
// Affinity: any thread; no shared state.
double block_level_db(std::span<const std::int16_t> samples);
double block_level_db(std::span<const float> samples);

} // namespace fv::dsp
