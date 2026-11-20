#include "ui_overlay/dots_look.h"

#include <vector>

#include <QPainter>

#include "uimath/voice_ribbon.h"

namespace fv {
namespace {

void drawDots(QPainter& painter, const std::vector<Dot>& dots) {
    painter.setPen(Qt::NoPen);
    for (const Dot& dot : dots) {
        const QPointF center = toQPointF(dot.center);
        painter.setBrush(toQColor(dot.halo_color));
        painter.drawEllipse(center, dot.halo_radius, dot.halo_radius);
        painter.setBrush(toQColor(dot.color));
        painter.drawEllipse(center, dot.radius, dot.radius);
    }
}

} // namespace

void DotsLook::draw(QPainter& painter, double grow, double width) {
    drawDots(painter, dots_frame(ribbon().history(), width, hue(), grow, darkTheme()));
}

void DotsLook::drawWork(QPainter& painter, double morph, double clock, double width) {
    drawDots(painter, dots_finish(snapshot(), width, hue(), morph, clock, darkTheme()));
}

} // namespace fv
