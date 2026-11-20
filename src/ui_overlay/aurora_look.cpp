#include "ui_overlay/aurora_look.h"

#include <vector>

#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include "uimath/voice_ribbon.h"

namespace fv {
namespace {

void drawWaves(QPainter& painter, const std::vector<AuroraWave>& waves) {
    painter.setBrush(Qt::NoBrush);
    for (const AuroraWave& wave : waves) {
        QPainterPath path;
        for (std::size_t i = 0; i < wave.points.size(); ++i) {
            if (i != 0) {
                path.lineTo(toQPointF(wave.points[i]));
            } else {
                path.moveTo(toQPointF(wave.points[i]));
            }
        }
        // A soft wide glow under a bright core.
        painter.setPen(
            QPen(toQColor(wave.glow_color), wave.glow_width, Qt::SolidLine, Qt::RoundCap));
        painter.drawPath(path);
        painter.setPen(
            QPen(toQColor(wave.core_color), wave.core_width, Qt::SolidLine, Qt::RoundCap));
        painter.drawPath(path);
    }
}

} // namespace

void AuroraLook::advance(double dt) {
    sim_.advance(dt, ribbon().shown());
}

void AuroraLook::draw(QPainter& painter, double grow, double width) {
    drawWaves(painter, sim_.frame(width, hue(), grow, darkTheme()));
}

void AuroraLook::drawWork(QPainter& painter, double morph, double clock, double width) {
    drawWaves(painter, sim_.finish(width, hue(), morph, clock, darkTheme()));
}

} // namespace fv
