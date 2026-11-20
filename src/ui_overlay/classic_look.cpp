#include "ui_overlay/classic_look.h"

#include <algorithm>

#include <QFont>
#include <QPainter>
#include <QPen>
#include <QRectF>

#include "ui_theme/palette.h"
#include "uimath/color.h"
#include "uimath/particles.h"

namespace fv {

QColor particleColor(const Particle& particle, std::optional<double> baseHue) {
    const double hue = baseHue.value_or(particle.color_hue);
    if (ui::currentPalette().isDark()) {
        return QColor::fromHsv(hue_degrees(hue), 200, static_cast<int>(particle.life * 230 + 25));
    }
    return QColor::fromHsv(hue_degrees(hue), 235, 205,
                           std::max(0, std::min(255, static_cast<int>(particle.life * 255))));
}

QColor particleFadingColor(const Particle& particle) {
    const int alpha = static_cast<int>(255 * particle.life);
    if (ui::currentPalette().isDark()) {
        return QColor::fromHsv(hue_degrees(particle.color_hue), 200, 230, alpha);
    }
    return QColor::fromHsv(hue_degrees(particle.color_hue), 235, 205, alpha);
}

void drawStyleParticles(QPainter& painter, const ParticleStyle& style) {
    painter.setPen(Qt::NoPen);
    for (const Particle& particle : style.particles()) {
        const QColor color = particleColor(particle);
        painter.setBrush(color);
        const double size = particle.size * particle.life;
        painter.drawEllipse(QRectF(particle.x - size, particle.y - size, size * 2, size * 2));
        if (style.config.glow_effect && particle.life > 0.5) {
            QColor glow(color);
            glow.setAlpha(kParticleGlowAlpha);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(glow, 1));
            const double glowSize = size + 1;
            painter.drawEllipse(
                QRectF(particle.x - glowSize, particle.y - glowSize, glowSize * 2, glowSize * 2));
            painter.setPen(Qt::NoPen);
        }
    }
}

void drawStyleState(QPainter& painter, const ParticleStyle& style, const QRect& rect,
                    const QString& message) {
    drawStyleParticles(painter, style);
    painter.setPen(ui::tokenColor("overlay-text"));
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 10, QFont::Bold));
    painter.drawText(QRect(0, rect.height() - 25, rect.width(), 20), Qt::AlignCenter, message);
}

} // namespace fv
