#include "ui_overlay/mirror_wave_look.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRectF>

namespace fv {
namespace {

QPainterPath outlinePath(const WaveShape& shape) {
    QPainterPath path(toQPointF(shape.outline.front()));
    for (std::size_t i = 1; i < shape.outline.size(); ++i) {
        path.lineTo(toQPointF(shape.outline[i]));
    }
    path.closeSubpath();
    return path;
}

QLinearGradient gradient(const WaveShape& shape, const std::vector<GradientStop>& stops) {
    QLinearGradient made(shape.gradient_left, 0, shape.gradient_right, 0);
    for (const GradientStop& stop : stops) {
        made.setColorAt(stop.position, toQColor(stop.color));
    }
    return made;
}

} // namespace

std::function<double(double)> MirrorWaveLook::emitter() const {
    const VoiceRibbon* voice = &ribbon();
    const double width = context().overlay_width;
    return [voice, width](double x) {
        return wave_emitter(*voice, x, width);
    };
}

void MirrorWaveLook::draw(QPainter& painter, double grow, double width) {
    const WaveShape shape = mirror_wave_frame(ribbon(), width, hue(), grow, darkTheme());
    const QPainterPath path = outlinePath(shape);
    painter.setPen(Qt::NoPen);
    painter.fillPath(path, gradient(shape, shape.fill));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QBrush(gradient(shape, shape.edge)), shape.edge_pen_width));
    painter.drawPath(path);
}

void MirrorWaveLook::drawWork(QPainter& painter, double morph, double clock, double width) {
    const WaveShape shape = mirror_wave_finish(snapshot(), width, hue(), morph, clock, darkTheme());
    painter.setPen(Qt::NoPen);
    painter.setBrush(toQColor(shape.pill_color));
    painter.drawRoundedRect(QRectF(shape.pill.x, shape.pill.y, shape.pill.width, shape.pill.height),
                            shape.pill_radius, shape.pill_radius);
    painter.setBrush(gradient(shape, shape.fill));
    painter.drawPath(outlinePath(shape));
}

} // namespace fv
