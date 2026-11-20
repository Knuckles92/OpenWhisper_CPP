#include "ui_overlay/ribbon_look.h"

#include <optional>

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include "uimath/voice_ribbon.h"

namespace fv {

double RibbonLook::baseline() const {
    return context().base_height - kBaselineFromBottom;
}

std::function<double(double)> RibbonLook::emitter() const {
    const VoiceRibbon* voice = &ribbon();
    const double width = context().overlay_width;
    const double line = baseline();
    return [voice, width, line](double x) {
        return voice->crest(x, width, line);
    };
}

void RibbonLook::draw(QPainter& painter, double grow, double width) {
    const std::optional<RibbonShape> shape =
        ribbon().shape(width, baseline(), hue(), grow, darkTheme());
    if (!shape || shape->top.empty()) {
        return;
    }
    QPainterPath area(QPointF(shape->top.front().x, shape->bottom_y));
    for (const PointD& point : shape->top) {
        area.lineTo(toQPointF(point));
    }
    area.lineTo(QPointF(shape->top.back().x, shape->bottom_y));
    area.closeSubpath();
    QLinearGradient fill(shape->gradient_left, 0, shape->gradient_right, 0);
    QLinearGradient edge(shape->gradient_left, 0, shape->gradient_right, 0);
    for (const GradientStop& stop : shape->fill) {
        fill.setColorAt(stop.position, toQColor(stop.color));
    }
    for (const GradientStop& stop : shape->edge) {
        edge.setColorAt(stop.position, toQColor(stop.color));
    }
    painter.setPen(Qt::NoPen);
    painter.fillPath(area, fill);
    QPainterPath line(toQPointF(shape->top.front()));
    for (std::size_t i = 1; i < shape->top.size(); ++i) {
        line.lineTo(toQPointF(shape->top[i]));
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QBrush(edge), shape->edge_pen_width, Qt::SolidLine, Qt::RoundCap));
    painter.drawPath(line);
}

void RibbonLook::drawWork(QPainter& painter, double morph, double clock, double width) {
    const RibbonFinish finish =
        ribbon_finish(snapshot(), width, baseline(), hue(), morph, clock, darkTheme());
    for (const LineSegment& segment : finish.segments) {
        painter.setPen(QPen(toQColor(segment.color), segment.width, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(toQPointF(segment.from), toQPointF(segment.to));
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(toQColor(finish.head_color));
    painter.drawEllipse(toQPointF(finish.head), finish.head_radius, finish.head_radius);
}

} // namespace fv
