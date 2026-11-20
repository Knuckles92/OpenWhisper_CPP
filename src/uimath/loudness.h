#pragma once

namespace fv {

// Levels are RMS fractions of full scale. Speech on a desk microphone sits around -40 to
// -15 dBFS, so this window gives it most of the range while room noise stays near zero.
inline constexpr double kLoudnessFloorDb = -60.0;
inline constexpr double kLoudnessCeilingDb = -12.0;

// An RMS level (0-1 of full scale) as a 0-1 loudness, on a dB scale so quiet speech still moves
// a display and a shout does not flatten everything else. A level <= 0 is 0.
double level_to_height(double level);

} // namespace fv
