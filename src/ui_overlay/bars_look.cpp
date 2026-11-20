#include "ui_overlay/bars_look.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QRectF>

#include "uimath/color.h"
#include "uimath/voice_ribbon.h"

namespace fv {
namespace {

// Python's float %: the result takes the divisor's sign.
double pyMod(double value, double modulus) {
    double result = std::fmod(value, modulus);
    if (result != 0.0 && ((result < 0.0) != (modulus < 0.0))) {
        result += modulus;
    }
    return result;
}

QPointF lerp(const QPointF& a, const QPointF& b, double t) {
    return a + (b - a) * t;
}

} // namespace

void BarsLook::draw(QPainter& painter, double grow, double width) {
    const std::vector<double> history = ribbon().history();
    const auto count = static_cast<long long>(history.size());
    const double span = width - 2 * kRibbonEdge;
    const long long slots = std::max(1LL, static_cast<long long>(std::floor(span / kPitch)));
    const double baseHue = hue();
    painter.setPen(Qt::NoPen);
    for (long long i = 0; i < slots; ++i) {
        // Newest at the right; each bar is one history sample. The operands are never negative,
        // so C++ division floors as Python's // does.
        const long long index = count - 1 - (slots - 1 - i) * count / slots;
        const double level = history[static_cast<std::size_t>(std::max(0LL, index))];
        const double x = kRibbonEdge + static_cast<double>(i) * kPitch;
        const double fade = detail::py_min(1.0, (x - kRibbonEdge) / (span * 0.25));
        const double half = (1.5 + kHalf * level) * grow;
        painter.setBrush(
            hueColor(baseHue + static_cast<double>(i) / static_cast<double>(slots) * 60,
                     (120 + 135 * level) * fade));
        painter.drawRoundedRect(QRectF(x, kCenterY - half, kBar, half * 2), kBar / 2, kBar / 2);
    }
}

void BarsLook::drawWork(QPainter& painter, double morph, double clock, double width) {
    const double w = width;
    const double cx = w / 2;
    const double cy = kCenterY - 1;
    const double head = pyMod(clock, 1.0);
    const double baseHue = hue();
    constexpr double tau = 2 * std::numbers::pi;
    for (int k = 0; k < kSpokes; ++k) {
        const double f = static_cast<double>(k) / kSpokes;
        const double h = snapAt(f);
        const double x0 = kRibbonEdge + (w - 2 * kRibbonEdge) * f;
        const double angle = f * tau - std::numbers::pi / 2;
        const double glow = detail::py_max(0.0, 1.0 - pyMod(f - head, 1.0) * 3.2);
        const double inner = 9.0;
        const double length = 4 + 9 * (0.35 * h + 0.65 * glow);
        // Each bar's ends travel to a spoke's inner and outer ends.
        const double cosA = std::cos(angle);
        const double sinA = std::sin(angle);
        const QPointF barTop(x0, kCenterY - (1.5 + kHalf * h));
        const QPointF barBottom(x0, kCenterY + (1.5 + kHalf * h));
        const QPointF spokeIn(cx + inner * cosA, cy + inner * sinA);
        const QPointF spokeOut(cx + (inner + length) * cosA, cy + (inner + length) * sinA);
        const QPointF a = lerp(barTop, spokeIn, morph);
        const QPointF b = lerp(barBottom, spokeOut, morph);
        const double alpha = 80 + 175 * detail::py_max(glow * morph, (1 - morph) * (0.4 + h));
        painter.setPen(QPen(hueColor(baseHue + f * 60, alpha), kBar, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(a, b);
    }
}

} // namespace fv
