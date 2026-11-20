#include "ui_overlay/pulse_look.h"

#include <vector>

#include <QConicalGradient>
#include <QPainter>
#include <QPen>

#include "uimath/voice_ribbon.h"

namespace fv {
namespace {

void drawRipples(QPainter& painter, const std::vector<PulseCircle>& ripples) {
    painter.setBrush(Qt::NoBrush);
    for (const PulseCircle& ripple : ripples) {
        painter.setPen(QPen(toQColor(ripple.pen), ripple.pen_width));
        painter.drawEllipse(toQPointF(ripple.center), ripple.radius, ripple.radius);
    }
}

void drawCore(QPainter& painter, const PulseCircle& core) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(toQColor(core.fill));
    painter.drawEllipse(toQPointF(core.center), core.radius, core.radius);
}

} // namespace

void PulseLook::advance(double dt) {
    sim_.advance(dt, ribbon().shown());
}

void PulseLook::beginWork() {
    Visual::beginWork();
    sim_.begin_work(ribbon().history());
}

void PulseLook::draw(QPainter& painter, double grow, double width) {
    const PulseFrame frame = sim_.frame(width, ribbon().shown(), hue(), grow, darkTheme());
    drawRipples(painter, frame.ripples);
    painter.setPen(QPen(toQColor(frame.ring.pen), frame.ring.pen_width));
    painter.setBrush(toQColor(frame.ring.fill));
    painter.drawEllipse(toQPointF(frame.ring.center), frame.ring.radius, frame.ring.radius);
    drawCore(painter, frame.core);
}

void PulseLook::drawWork(QPainter& painter, double morph, double clock, double width) {
    const PulseFinish finish = sim_.finish(width, hue(), morph, clock, darkTheme());
    const QPointF center = toQPointF(finish.ring.center);
    drawRipples(painter, finish.ripples);
    QConicalGradient sweep(center, finish.sweep_angle_degrees);
    for (const GradientStop& stop : finish.sweep) {
        sweep.setColorAt(stop.position, toQColor(stop.color));
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(toQColor(finish.ring.pen), finish.ring.pen_width));
    painter.drawEllipse(center, finish.ring.radius, finish.ring.radius);
    painter.setPen(QPen(QBrush(sweep), finish.sweep_pen_width, Qt::SolidLine, Qt::RoundCap));
    painter.drawEllipse(center, finish.ring.radius, finish.ring.radius);
    drawCore(painter, finish.core);
}

} // namespace fv
