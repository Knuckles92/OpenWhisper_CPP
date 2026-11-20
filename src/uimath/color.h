#pragma once

namespace fv {

// A colour as QColor reports it: 8-bit channels, alpha 255 opaque.
struct Rgba {
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 255;
    friend bool operator==(const Rgba&, const Rgba&) = default;
};

// QColor::fromHsv's arguments: hue 0-359 degrees, saturation and value 0-255.
struct Hsv {
    int h = 0;
    int s = 0;
    int v = 0;
    friend bool operator==(const Hsv&, const Hsv&) = default;
};

// A point in the overlay's logical pixels, with QPointF's meaning.
struct PointD {
    double x = 0.0;
    double y = 0.0;
    friend bool operator==(const PointD&, const PointD&) = default;
};

// QColor::fromHsv(hsv, alpha) read back as RGB, with the channel arithmetic of Qt 6 (16-bit
// channels held as 0x0101 times the 8-bit value, float maths, each channel read back rounded).
// Arguments outside their ranges are clamped, where QColor would return an invalid colour.
Rgba to_rgba(Hsv hsv, int alpha = 255);

// int(hue) % 360 as Python computes it: truncated toward zero, then reduced to 0-359 (a
// negative hue wraps up). Any finite hue is defined, however large; NaN and infinities read 0.
int hue_degrees(double hue);

// max(0, min(255, int(alpha))): the alpha of a colour built from a float.
int clamp_alpha(double alpha);

namespace detail {
// Python's min(a, b) and max(a, b) keep the first argument unless the second compares past it,
// so a NaN second argument never wins.
inline double py_min(double a, double b) {
    return (b < a) ? b : a;
}
inline double py_max(double a, double b) {
    return (b > a) ? b : a;
}
} // namespace detail

} // namespace fv
