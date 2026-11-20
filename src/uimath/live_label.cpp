#include "uimath/live_label.h"

#include <cmath>
#include <format>
#include <limits>

namespace fv {

std::string format_clock(double seconds) {
    // int() truncates toward zero; max(0, ...) then drops the negatives.
    const double truncated = std::trunc(seconds);
    long long whole = 0;
    if (truncated >= 9.0e18) {
        whole = std::numeric_limits<long long>::max();
    } else if (truncated > 0.0) {
        whole = static_cast<long long>(truncated);
    }
    return std::format("{}:{:02d}", whole / 60, whole % 60);
}

double particle_hue(double animation_time, double color_shift_speed) {
    double hue = std::fmod(animation_time * color_shift_speed, 360.0);
    if (hue < 0.0) {
        hue += 360.0;
    }
    return hue;
}

} // namespace fv
