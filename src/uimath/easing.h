#pragma once

namespace fv {

// How long the contents of a moment fade out before the overlay hides.
inline constexpr double kMomentFadeS = 0.3;
// ease_out_back's overshoot where nothing else is named: the disc pop.
inline constexpr double kDefaultOvershoot = 2.2;

// How far now is through the window [start, start + length], 0-1. length must not be 0.
double phase(double now, double start, double length);

double ease_out_cubic(double t);

// Runs past 1 and settles back, so a shape pops rather than slides. The overshoot differs per
// use (the disc pop 2.2, the slammed disc 3.0).
double ease_out_back(double t, double overshoot = kDefaultOvershoot);

// 1 until the last kMomentFadeS of a moment lasting total seconds.
double exit_opacity(double now, double total);

// A head-shake: a horizontal wobble that dies away, for a "no". 0 outside (start, start +
// length).
double shake_offset(double now, double start, double length, double amplitude, double wiggles);

} // namespace fv
