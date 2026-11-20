#include "ui_overlay/ocean_looks.h"

#include <cmath>
#include <numbers>
#include <vector>

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRectF>

#include "uimath/voice_ribbon.h"

namespace fv {
namespace {

// Python's float % for a positive modulus: the result is never negative.
double pyMod(double value, double modulus) {
    double result = std::fmod(value, modulus);
    if (result < 0.0) {
        result += modulus;
    }
    return result;
}

QColor withAlpha(const QColor& color, int alpha) {
    QColor copy(color);
    copy.setAlpha(alpha);
    return copy;
}

} // namespace

// --- the school ------------------------------------------------------------------------------

SchoolLook::SchoolLook(const LookContext& context, SchoolKind kind)
    : Visual(context), sim_(kind, static_cast<double>(context.overlay_width)) {}

void SchoolLook::advance(double dt) {
    sim_.advance(dt, ribbon().shown());
}

void SchoolLook::beginWork() {
    Visual::beginWork();
    sim_.begin_work();
}

void SchoolLook::draw(QPainter& painter, double grow, double /*width*/) {
    drawSchool(painter, grow);
}

void SchoolLook::drawWork(QPainter& painter, double /*morph*/, double /*clock*/, double width) {
    draw(painter, 1.0, width);
}

QPainterPath SchoolLook::layerPath(int layer) const {
    const std::vector<PointD> points = sim_.layer_points(layer);
    QPainterPath path(toQPointF(points.front()));
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const PointD& a = points[i];
        const PointD& b = points[i + 1];
        path.quadTo(toQPointF(a), QPointF((a.x + b.x) / 2, (a.y + b.y) / 2));
    }
    path.lineTo(toQPointF(points.back()));
    return path;
}

QColor SchoolLook::ocean(int layer, double alpha, double light) const {
    return toQColor(sim_.ocean_color(layer, alpha, light, darkTheme()));
}

QLinearGradient SchoolLook::faded(int layer, double alpha, double light) const {
    QLinearGradient gradient(kRibbonEdge, 0, sim_.width() - kRibbonEdge, 0);
    struct Stop {
        double position;
        double share;
    };
    for (const Stop stop : {Stop{0.0, 0.0}, Stop{0.07, 1.0}, Stop{0.93, 1.0}, Stop{1.0, 0.0}}) {
        gradient.setColorAt(stop.position, ocean(layer, alpha * stop.share, light));
    }
    return gradient;
}

QColor SchoolLook::fishColor(const Fish& fish, double alpha) const {
    return toQColor(sim_.fish_color(fish, alpha, hue(), darkTheme()));
}

void SchoolLook::drawLines(QPainter& painter, double grow, double strength) const {
    painter.setBrush(Qt::NoBrush);
    for (int layer = 0; layer < SchoolSim::kLayers; ++layer) {
        const QPainterPath path = layerPath(layer);
        painter.setPen(
            QPen(QBrush(faded(layer, 60 * grow * strength)), 4.5, Qt::SolidLine, Qt::RoundCap));
        painter.drawPath(path);
        painter.setPen(QPen(QBrush(faded(layer, (150 + 20 * layer) * grow * strength)), 1.6,
                            Qt::SolidLine, Qt::RoundCap));
        painter.drawPath(path);
    }
}

void SchoolLook::drawBands(QPainter& painter, double grow, double light) const {
    for (int layer = SchoolSim::kLayers - 1; layer >= 0; --layer) {
        const QPainterPath path = layerPath(layer);
        QPainterPath body(path);
        body.lineTo(QPointF(sim_.width() - kRibbonEdge, SchoolSim::kBase));
        body.lineTo(QPointF(kRibbonEdge, SchoolSim::kBase));
        body.closeSubpath();
        painter.setPen(Qt::NoPen);
        painter.fillPath(body, faded(layer, (34 + 6 * layer + 12 * light) * grow, light));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(
            QPen(QBrush(faded(layer, (190 + 65 * light) * grow, 1.0)), 1.3 + 0.5 * light));
        painter.drawPath(path);
    }
}

void SchoolLook::drawBalls(QPainter& painter, double grow) const {
    const double voice = sim_.voice();
    for (const Fish& f : sim_.fish()) {
        const QColor color = fishColor(f, 255 * grow);
        const QPointF center(f.x, f.y);
        painter.setPen(Qt::NoPen);
        if (voice > 0.05) {
            painter.setBrush(withAlpha(color, static_cast<int>(70 * voice * grow)));
            painter.drawEllipse(center, f.r + 2.5 * voice, f.r + 2.5 * voice);
        }
        painter.setBrush(color);
        painter.drawEllipse(center, f.r, f.r);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(withAlpha(color, static_cast<int>((100 + 120 * voice) * grow)), 1));
        painter.drawEllipse(center, f.r + 1, f.r + 1);
    }
}

void BaitBallLook::drawSchool(QPainter& painter, double grow) {
    drawBalls(painter, grow);
    drawLines(painter, grow);
}

void RealFishLook::drawSchool(QPainter& painter, double grow) {
    drawBands(painter, grow, sim().voice());
    drawFish(painter, grow);
}

void RealFishLook::drawFish(QPainter& painter, double grow) const {
    // Speaking, the school swims harder: bigger, quicker tails.
    const double voice = sim().voice();
    const double size = 1.0 + 0.3 * voice;
    for (const Fish& f : sim().fish()) {
        const double angle = std::atan2(f.vy, std::abs(f.vx) > 0.1 ? f.vx : 0.1);
        const double r = f.r * size;
        const double length = 2.4 + r * 1.6;
        painter.save();
        painter.translate(f.x, f.y);
        painter.rotate(angle * 180.0 / std::numbers::pi);
        QPainterPath body;
        body.moveTo(length, 0);
        body.quadTo(0, -r * 0.9, -length * 0.6, 0);
        body.quadTo(0, r * 0.9, length, 0);
        const double wag =
            std::sin(sim().time() * (10 + 14 * voice) + f.phase * 5) * r * (0.45 + 0.3 * voice);
        QPainterPath tail;
        tail.moveTo(-length * 0.5, 0);
        tail.lineTo(-length * 1.05, -r * 0.8 + wag);
        tail.lineTo(-length * 1.05, r * 0.8 + wag);
        tail.closeSubpath();
        painter.setPen(Qt::NoPen);
        painter.setBrush(fishColor(f, 240 * grow));
        painter.drawPath(body);
        painter.setBrush(fishColor(f, 170 * grow));
        painter.drawPath(tail);
        painter.restore();
    }
}

void RealFishLook::drawWork(QPainter& painter, double morph, double clock, double width) {
    draw(painter, 1.0, width);
    // Light shimmering across the water, like caustics.
    const double x = kRibbonEdge + (sim().width() - 2 * kRibbonEdge) * pyMod(clock * 0.8, 1.0);
    painter.setBrush(Qt::NoBrush);
    for (int layer = 0; layer < SchoolSim::kLayers; ++layer) {
        QLinearGradient shimmer(x - 50, 0, x + 50, 0);
        shimmer.setColorAt(0.0, ocean(layer, 0, 1.0));
        shimmer.setColorAt(0.5, ocean(layer, 230 * morph, 1.0));
        shimmer.setColorAt(1.0, ocean(layer, 0, 1.0));
        painter.setPen(QPen(QBrush(shimmer), 2.0));
        painter.drawPath(layerPath(layer));
    }
}

void DeepGlowLook::drawSchool(QPainter& painter, double grow) {
    drawGlow(painter, grow);
    drawLines(painter, grow, 0.6);
}

void DeepGlowLook::drawGlow(QPainter& painter, double grow) const {
    for (const Fish& f : sim().fish()) {
        const std::vector<PointD>& trail = f.trail;
        for (std::size_t k = 1; k < trail.size(); ++k) {
            const double share = static_cast<double>(k) / static_cast<double>(trail.size());
            painter.setPen(
                QPen(fishColor(f, 110 * share * grow), f.r * share, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(toQPointF(trail[k - 1]), toQPointF(trail[k]));
        }
        const double twinkle = 0.65 + 0.35 * std::sin(sim().time() * 6 + f.phase * 3);
        const QPointF center(f.x, f.y);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fishColor(f, 70 * twinkle * grow));
        painter.drawEllipse(center, f.r + 2.5, f.r + 2.5);
        painter.setBrush(fishColor(f, 255 * twinkle * grow));
        painter.drawEllipse(center, f.r * 0.8, f.r * 0.8);
    }
}

void DeepGlowLook::drawWork(QPainter& painter, double morph, double clock, double /*width*/) {
    const QPointF center(sim().width() / 2, SchoolSim::kBase - 15);
    // Sonar pings ripple out through the dimmed water.
    painter.setBrush(Qt::NoBrush);
    for (int k = 0; k < 3; ++k) {
        const double age = pyMod(clock / SchoolSim::kDeepGlowPingS + k / 3.0, 1.0);
        painter.setPen(
            QPen(fishColor(sim().fish().front(), 170 * std::pow(1 - age, 2) * morph), 1.2));
        painter.drawEllipse(center, 8 + 120 * age, 4 + 28 * age);
    }
    drawGlow(painter, 1.0);
    drawLines(painter, 1.0, 0.6 - 0.35 * morph);
}

// --- Fizz ------------------------------------------------------------------------------------

FizzLook::FizzLook(const LookContext& context)
    : Visual(context), sim_(static_cast<double>(context.overlay_width)) {}

void FizzLook::advance(double dt) {
    sim_.advance(dt, ribbon().shown());
}

void FizzLook::beginWork() {
    Visual::beginWork();
    sim_.begin_work();
}

void FizzLook::draw(QPainter& painter, double grow, double /*width*/) {
    drawGlasses(painter, sim_.levels(), grow, 9.0);
    drawBubbles(painter, grow);
}

void FizzLook::drawWork(QPainter& painter, double morph, double clock, double /*width*/) {
    // The levels ease from where they were into a wave rolling across.
    const FizzSim::Levels levels = sim_.work_levels(morph, clock);
    drawGlasses(painter, levels, 1.0, 9.0 + 6.0 * morph);
    drawBubbles(painter, 1.0);
}

void FizzLook::drawGlasses(QPainter& painter, const FizzSim::Levels& levels, double grow,
                           double foamSpeed) const {
    const double w = sim_.pitch() - 8;
    const double baseHue = hue();
    const bool dark = darkTheme();
    for (int i = 0; i < FizzSim::kLanes; ++i) {
        const double h = levels[static_cast<std::size_t>(i)];
        const double x = sim_.lane_x(i);
        const QRectF glass(x - w / 2, FizzSim::kTop, w, FizzSim::kBase - FizzSim::kTop);
        const double level = FizzSim::kBase - h;
        const double laneHue = baseHue + static_cast<double>(i) / FizzSim::kLanes * 60;
        QLinearGradient fill(0, level, 0, FizzSim::kBase);
        fill.setColorAt(0.0, toQColor(hue_color(laneHue, 190 * grow, true, dark)));
        fill.setColorAt(1.0, toQColor(hue_color(laneHue, 70 * grow, true, dark)));
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRect(QRectF(glass.left() + 1, level, w - 2, FizzSim::kBase - level));
        for (int k = 0; k < 4; ++k) {
            const double foamX = glass.left() + 3 + k * (w - 6) / 3;
            painter.setBrush(toQColor(hue_color(baseHue + 40, 200 * grow, true, dark)));
            const double foamRx = 1.8 + 0.6 * std::sin(sim_.time() * foamSpeed + k + i);
            painter.drawEllipse(QPointF(foamX, level - 1), foamRx, 1.6);
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(toQColor(hue_color(laneHue, 150 * grow, true, dark)), 1));
        painter.drawRoundedRect(glass, 3, 3);
    }
}

void FizzLook::drawBubbles(QPainter& painter, double grow) const {
    const double baseHue = hue();
    const bool dark = darkTheme();
    painter.setPen(Qt::NoPen);
    for (const FizzSim::Bubble& b : sim_.bubbles()) {
        if (b.y < FizzSim::kBase - sim_.levels()[static_cast<std::size_t>(b.lane)] - 1) {
            continue; // above the surface it has already popped
        }
        painter.setBrush(toQColor(hue_color(baseHue + 30, 230 * grow, true, dark)));
        painter.drawEllipse(QPointF(b.x, b.y), b.r, b.r);
    }
    painter.setBrush(Qt::NoBrush);
    for (const FizzSim::Splash& splash : sim_.splashes()) {
        const double age = (sim_.time() - splash.start) / FizzSim::kSplashS;
        painter.setPen(
            QPen(toQColor(hue_color(baseHue + 40, 200 * (1 - age) * grow, true, dark)), 1));
        painter.drawEllipse(QPointF(splash.x, splash.y), 2 + 5 * age, 1 + 2 * age);
    }
}

} // namespace fv
