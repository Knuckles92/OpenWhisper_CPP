#pragma once

#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

#include <QColor>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QRectF>
#include <QString>

#include "uimath/moments.h"

class QPainter;

namespace fv {

// The painting of the overlay's one-shot moments (ui_qt/overlays/moments.py): a lead-in, a disc
// popping in the moment's tone with rings rolling out and the pill's edge flashing, a glyph that
// draws itself cut out of the disc, confetti, and a label rising under a glint. Times are seconds
// into the moment; tones are palette tokens. The timings and tables are uimath/moments.h.
// Affinity: GUI thread.

// A pen with round caps and joins, so drawn glyph strokes look polished.
QPen roundPen(const QColor& color, double width);

void drawSparkle(QPainter& painter, double cx, double cy, double size, const QColor& color);
// The rewriting sparkles flying into center and shrinking.
void drawGatheringSparkles(QPainter& painter, const QRectF& rect, const QPointF& center, double now,
                           double length);
// Dots in the moment's two colours spiralling into center.
void drawConverging(QPainter& painter, const QPointF& center, double now, const Moment& moment);

// One dot of what was on screen when a moment began, as Canceled snapshots it.
struct CollapsingDot {
    double x = 0.0;
    double y = 0.0;
    QColor color;
    double radius = 0.0;
};
// What was on screen sucked into center.
void drawCollapsing(QPainter& painter, const QPointF& center, double now, double length,
                    const std::vector<CollapsingDot>& snapshot);
// The whole pill washes in tone with a bright edge, then clears.
void drawPillFlash(QPainter& painter, const QRectF& rect, double now, double start,
                   std::string_view tone);
// A bright core where everything imploded.
void drawCoreFlash(QPainter& painter, const QPointF& center, double now, double start);
// A disc that slams in from oversize, the reverse of a pop, then breathes.
void drawSlamDisc(QPainter& painter, const QPointF& center, double now, double start,
                  std::string_view tone);
void drawBubbles(QPainter& painter, const std::vector<Particle>& particles);
// Rings rolling out from the disc, one per tone, a beat apart.
void drawRings(QPainter& painter, const QPointF& center, double now, double start,
               std::initializer_list<std::string_view> tones, double reach = 20.0,
               double radius = kMomentRadius);
// The pill's edge lights up in tone and fades.
void drawEdgeFlash(QPainter& painter, const QRectF& rect, double now, double start,
                   std::string_view tone);
// A solid disc that pops in, then breathes inside a soft halo.
void drawDisc(QPainter& painter, const QPointF& center, double now, double start,
              std::string_view tone, double radius = kMomentRadius);
// The glyph's first amount (0-1) of length, as if being drawn.
QPainterPath glyphPainterPath(std::string_view glyph, const QPointF& center, double amount);
// The glyph stroke by stroke, cut out of the disc: it takes the pill's own colour.
void drawGlyph(QPainter& painter, const QPointF& center, double now, double start,
               std::string_view glyph);
void drawAfterglow(QPainter& painter, const QPointF& center, double now, double start);
// The label rises into place; one glint in the glint tone passes over it. slide is where it
// rises from (6 px below by default).
void drawMomentLabel(QPainter& painter, const QRectF& rect, const QString& text, double now,
                     double start, std::string_view glint, Qt::Alignment align = Qt::AlignCenter,
                     std::optional<QPointF> slide = std::nullopt);
// Solid dots: glow rings on dots this small read as bubbles.
void drawConfetti(QPainter& painter, const std::vector<Particle>& particles);

} // namespace fv
