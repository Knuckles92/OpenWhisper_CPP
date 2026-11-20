#include "uimath/overlay_look.h"

#include <algorithm>

namespace fv {

std::string_view normalize_look(std::string_view value) {
    if (value == "live") {
        value = kLookRibbon;
    } else if (value == "orb") {
        value = kLookPulse;
    } else if (value == "bubbles") {
        value = kLookBaitBall;
    }
    const auto found = std::ranges::find(kLooks, value);
    return found == kLooks.end() ? kDefaultLook : *found;
}

double smooth(double t) {
    t = detail::py_max(0.0, detail::py_min(1.0, t));
    return t * t * (3.0 - 2.0 * t);
}

Rgba hue_color(double hue, double alpha, bool bright, bool dark) {
    const int degrees = hue_degrees(hue);
    Hsv hsv;
    if (dark) {
        hsv = Hsv{degrees, bright ? 190 : 150, 245};
    } else {
        hsv = Hsv{degrees, 235, 205};
    }
    return to_rgba(hsv, clamp_alpha(alpha));
}

PointD lerp_point(PointD a, PointD b, double t) {
    return PointD{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

} // namespace fv
