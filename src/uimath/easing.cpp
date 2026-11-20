#include "uimath/easing.h"

#include <cmath>
#include <numbers>

#include "uimath/color.h"

namespace fv {

double phase(double now, double start, double length) {
    return detail::py_max(0.0, detail::py_min(1.0, (now - start) / length));
}

double ease_out_cubic(double t) {
    return 1.0 - std::pow(1.0 - t, 3.0);
}

double ease_out_back(double t, double overshoot) {
    t -= 1.0;
    return 1.0 + (overshoot + 1.0) * std::pow(t, 3.0) + overshoot * std::pow(t, 2.0);
}

double exit_opacity(double now, double total) {
    return phase(total - now, 0.0, kMomentFadeS);
}

double shake_offset(double now, double start, double length, double amplitude, double wiggles) {
    const double shake = phase(now, start, length);
    if (!(0.0 < shake && shake < 1.0)) {
        return 0.0;
    }
    return amplitude * std::sin(shake * (2.0 * std::numbers::pi) * wiggles) * (1.0 - shake);
}

} // namespace fv
