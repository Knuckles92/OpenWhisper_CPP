#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <QColor>
#include <QFont>
#include <QList>
#include <QPainterPath>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QString>

class QPainter;

// What Snap draws: callouts, strokes, labels, steps and opaque redactions
// (ui_qt/snap/annotations.py). Annotations are small vector objects in logical pixels; the
// overlay paints them on screen, and the export paints the very same objects onto the
// device-pixel crop with the painter scaled by the device-pixel ratio, so what the user drew is
// what they keep, crisp at any scale.
namespace fv::ui_snap {

enum class SnapTool { Arrow, Box, Ellipse, Pen, Highlighter, Text, Step, Redact };

// Tool's value as Python's StrEnum spells it ("arrow", "highlighter").
const char* toolName(SnapTool tool);

struct Swatch {
    const char16_t* name;
    const char* color; // "#RRGGBB"
};

// Saturated mid-tones that hold up on light and dark apps alike; coral first.
inline constexpr std::array<Swatch, 4> kSwatches{{
    {u"Coral", "#FF4F3A"},
    {u"Amber", "#F5A524"},
    {u"Blue", "#3A7BFD"},
    {u"Green", "#1FA463"},
}};

// Drags shorter than this are clicks, not shapes.
inline constexpr double kMinDrag = 6.0;
inline constexpr double kArrowWidth = 4.0;
inline constexpr double kBoxWidth = 3.0;
inline constexpr double kPenWidth = 4.0;
inline constexpr double kHighlighterWidth = 18.0;
inline constexpr int kHighlighterAlpha = 120;
inline constexpr int kBoxFillAlpha = 24;
inline constexpr double kBoxRadius = 6.0;
inline constexpr char kRedactionColor[] = "#181A20";
inline constexpr double kStepRadius = 13.0;
inline constexpr double kStepRing = 2.0;
inline constexpr double kLabelPadX = 8.0;
inline constexpr double kLabelPadY = 3.0;
inline constexpr double kLabelRadius = 6.0;
// How far an arrow bows, as a share of its length, and the most it ever bows.
inline constexpr double kArrowBend = 0.1;
inline constexpr double kArrowMaxBend = 26.0;

// Type that reads on a fill of color: white unless the fill is light (relative luminance of
// 0.3 or more), then #2A1A06. White stays on coral although dark type measures a little higher
// contrast there: it is how numbered badges look everywhere else.
QColor inkFor(const QColor& color);

// font without subpixel (ClearType) smoothing: kept images get scaled and viewed on other
// screens, where subpixel smoothing shows as coloured fringes around every letter.
QFont grayscale(const QFont& font);
// The application font, grayscale, at pixel_size px DemiBold.
QFont labelFont(int pixelSize = 16);
// The application font, grayscale, Bold: 14 px below 100, 11 px from 100.
QFont stepFont(int number);

// The shaft (a gently bowed quadratic curve) and the filled head of an arrow. The shaft stops
// inside the head, so its round cap never pokes past the tip. A zero-length arrow is a path
// at start and no head.
std::pair<QPainterPath, QPolygonF> arrowShapes(QPointF start, QPointF end, double width);
// Shaft and head as one outline: no doubled shadow where they meet.
QPainterPath arrowOutline(QPointF start, QPointF end, double width);
// A freehand line through points (at least one), smoothed by quadratics through midpoints.
QPainterPath smoothPath(std::span<const QPointF> points);

enum class AnnotationKind { Arrow, Box, Ellipse, Redaction, Stroke, Label, Step };

// One mark: Python's Arrow, Box, Ellipse, Redaction, Stroke, Label or Step, as one value type.
// Each kind reads only its own members.
struct Annotation {
    AnnotationKind kind = AnnotationKind::Arrow;
    QPointF start; // Arrow, Box, Ellipse, Redaction
    QPointF end;
    QColor color;             // everything but Redaction
    double width = 0.0;       // Arrow, Box, Ellipse: the line width
    bool infill = false;      // Box, Ellipse
    QList<QPointF> points;    // Stroke
    bool highlighter = false; // Stroke
    QPointF pos;              // Label: top-left of its tag
    QString text;             // Label
    QFont font;               // Label
    QPointF center;           // Step
    int number = 0;           // Step

    static Annotation arrow(QPointF start, QPointF end, const QColor& color,
                            double width = kArrowWidth);
    static Annotation box(QPointF start, QPointF end, const QColor& color, double width = kBoxWidth,
                          bool infill = false);
    // A circle or oval callout, sharing the box's optional light fill.
    static Annotation ellipse(QPointF start, QPointF end, const QColor& color,
                              double width = kBoxWidth, bool infill = false);
    // An opaque cover baked into kept screenshots, independent of colour and fill.
    static Annotation redaction(QPointF start, QPointF end);
    // A pen line, or with highlighter a wide translucent marker swipe.
    static Annotation stroke(QList<QPointF> points, const QColor& color, bool highlighter = false);
    // A short note on a rounded tag of the chosen colour, like the step badges.
    static Annotation label(QPointF pos, QString text, const QColor& color);
    static Annotation label(QPointF pos, QString text, const QColor& color, const QFont& font);
    // A numbered badge (filled circle, white ring), for "click here, then here".
    static Annotation step(QPointF center, int number, const QColor& color);

    friend bool operator==(const Annotation&, const Annotation&) = default;
};

// A Stroke's width: kHighlighterWidth or kPenWidth.
double strokeWidth(const Annotation& stroke);
// The normalized start-end rectangle of a Box, Ellipse or Redaction; a Label's tag.
QRectF annotationRect(const Annotation& item);
// Moves the drag end to point; a Stroke adds point unless it is under 1.5 px from the last one
// (sub-pixel jitter only adds bumps to the smoothed curve); Label and Step ignore it.
void extendAnnotation(Annotation& item, QPointF point);
// Whether a finished drag is a mark: arrows 6 px long; boxes and ellipses 3 px each way and 6
// px corner to corner; redactions 3 px each way; strokes always (a click is a dot); labels with
// text other than white space; steps always.
bool meaningful(const Annotation& item);
// Every pixel the item paints: the overlay repaints only this while drawing.
QRectF annotationBounds(const Annotation& item);
void paintAnnotation(QPainter& painter, const Annotation& item);

// The shape a tool drag starts at point; nullopt for Text and Step. Box and Ellipse take
// boxInfill; Redact ignores the colour.
std::optional<Annotation> newDraft(SnapTool tool, QPointF point, const QColor& color,
                                   bool boxInfill = false);
// Extends a drag; constrained (Shift) makes squares and circles or 45-degree arrow bearings,
// kept inside bounds when given.
void extendDraft(Annotation& draft, QPointF point, bool constrained = false,
                 std::optional<QRectF> bounds = std::nullopt);

// The marks on one snap, in drawing order, with undo and redo. Affinity: one thread.
class AnnotationStack {
public:
    std::span<const Annotation> items() const { return items_; }
    std::size_t size() const { return items_.size(); }
    bool canUndo() const { return !items_.empty(); }
    bool canRedo() const { return !undone_.empty(); }

    void add(Annotation item);
    std::optional<Annotation> undo();
    std::optional<Annotation> redo();
    // Steps count up from 1; undoing a step gives its number back.
    int nextStepNumber() const;

private:
    std::vector<Annotation> items_;
    std::vector<Annotation> undone_;
};

} // namespace fv::ui_snap
