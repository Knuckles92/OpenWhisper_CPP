#include "uimath/color.h"

#include <algorithm>
#include <climits>
#include <cmath>

namespace fv {
namespace {

// qRound for the non-negative values used here.
int round_half_up(float value) {
    return static_cast<int>(value + 0.5f);
}

// QColor reports a 16-bit channel as qRound(value / 257.0).
int to_byte(int channel16) {
    return static_cast<int>(static_cast<double>(channel16) / 257.0 + 0.5);
}

} // namespace

int hue_degrees(double hue) {
    // Python raises on int(nan) and int(inf); a colour is still owed here, so those read 0.
    if (!std::isfinite(hue)) {
        return 0;
    }
    // fmod is exact, so this equals int(hue) % 360 for any finite hue without converting a
    // value past the range of an integer type (an animation clock times a speed can grow far).
    const double mod = std::fmod(std::trunc(hue), 360.0);
    return static_cast<int>(mod < 0.0 ? mod + 360.0 : mod);
}

int clamp_alpha(double alpha) {
    if (!(alpha > 0.0)) {
        return 0;
    }
    if (alpha >= 255.0) {
        return 255;
    }
    return static_cast<int>(alpha);
}

Rgba to_rgba(Hsv hsv, int alpha) {
    const int h = std::clamp(hsv.h, 0, 359);
    const int s = std::clamp(hsv.s, 0, 255);
    const int v = std::clamp(hsv.v, 0, 255);
    Rgba out;
    out.a = std::clamp(alpha, 0, 255);
    const int saturation16 = s * 0x0101;
    const int value16 = v * 0x0101;
    if (saturation16 == 0) {
        out.r = out.g = out.b = to_byte(value16);
        return out;
    }
    const float hue = static_cast<float>(h * 100) / 6000.0f;
    const float sat = static_cast<float>(saturation16) / static_cast<float>(USHRT_MAX);
    const float val = static_cast<float>(value16) / static_cast<float>(USHRT_MAX);
    const int sector = static_cast<int>(hue);
    const float fraction = hue - static_cast<float>(sector);
    const float p = val * (1.0f - sat);
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    if ((sector & 1) != 0) {
        const float q = val * (1.0f - (sat * fraction));
        switch (sector) {
        case 1:
            red = q;
            green = val;
            blue = p;
            break;
        case 3:
            red = p;
            green = q;
            blue = val;
            break;
        default:
            red = val;
            green = p;
            blue = q;
            break;
        }
    } else {
        const float t = val * (1.0f - (sat * (1.0f - fraction)));
        switch (sector) {
        case 0:
            red = val;
            green = t;
            blue = p;
            break;
        case 2:
            red = p;
            green = val;
            blue = t;
            break;
        default:
            red = t;
            green = p;
            blue = val;
            break;
        }
    }
    out.r = to_byte(round_half_up(red * static_cast<float>(USHRT_MAX)));
    out.g = to_byte(round_half_up(green * static_cast<float>(USHRT_MAX)));
    out.b = to_byte(round_half_up(blue * static_cast<float>(USHRT_MAX)));
    return out;
}

} // namespace fv
