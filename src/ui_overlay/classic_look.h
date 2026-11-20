#pragma once

#include <optional>

#include <QColor>
#include <QRect>
#include <QString>

class QPainter;

namespace fv {

struct Particle;
class ParticleStyle;

// Classic, the original particles fed raw RMS (ui_qt/waveform_styles/particle_style.py's
// painting; the simulation is uimath's ParticleStyle). Affinity: GUI thread.

// Particle.get_qcolor: dying particles darken into the pill on the dark theme and fade out on
// the light one, where they run more saturated to hold against white. base_hue replaces the
// particle's own hue.
QColor particleColor(const Particle& particle, std::optional<double> baseHue = std::nullopt);
// Particle.get_fading_color: fades out by alpha on both themes.
QColor particleFadingColor(const Particle& particle);

// The style's particles, each with a glow ring while it is young.
void drawStyleParticles(QPainter& painter, const ParticleStyle& style);
// The particles, then message in the style's own status row at the foot of rect.
void drawStyleState(QPainter& painter, const ParticleStyle& style, const QRect& rect,
                    const QString& message);

} // namespace fv
