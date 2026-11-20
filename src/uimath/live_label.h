#pragma once

#include <string>

namespace fv {

// The live label's dot, and the gap between its parts, in pixels.
inline constexpr double kLiveDotDiameter = 8.0;
inline constexpr double kLiveLabelGap = 7.0;
// The dot's pulse ring repeats this often once the dot has settled.
inline constexpr double kLivePulseS = 1.4;

// The elapsed time as "m:ss": whole seconds (the fraction is dropped), negative values 0,
// minutes not capped ("61:01").
std::string format_clock(double seconds);

// The hue the particles are emitted around after animation_time seconds: the animation time
// times color_shift_speed, wrapped to 0-360.
double particle_hue(double animation_time, double color_shift_speed);

} // namespace fv
