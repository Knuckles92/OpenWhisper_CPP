#pragma once

#include <optional>
#include <span>

#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>

// Where things go on the snap overlay, and how logical pixels map to the frozen image
// (ui_qt/snap/geometry.py). The overlay works in logical pixels, the units the mouse moves in;
// the frozen screenshot is kept in device pixels. Qt's inclusive right() and bottom() are part
// of the maths, as in Python, so these keep QRect.
namespace fv::ui_snap {

// Space between the selection and the toolbar.
inline constexpr int kToolbarGap = 10;
// Floating chrome never comes closer than this to a screen edge.
inline constexpr int kEdgeMargin = 8;
// A selection edge catches the pointer this far outside it, and this far in: little inside,
// so marks can still start right by the frame.
inline constexpr int kGrabOutside = 10;
inline constexpr int kGrabInside = 4;
// Along an edge, this close to the end grabs the corner (the drawn corner mark).
inline constexpr int kGrabCorner = 16;
inline constexpr int kSnapDistance = 10;

// Which edges a grab moves: x is -1 (left), 1 (right) or 0; y likewise.
struct Grab {
    int x = 0;
    int y = 0;
    friend bool operator==(const Grab&, const Grab&) = default;
};

// The rectangle a drag from a to b covers: |dx| by |dy| pixels.
QRect span(QPoint a, QPoint b);

// Magnetizes point to edges of targets within kSnapDistance, favouring a corner when both
// axes are close.
QPointF snapPoint(QPointF point, std::span<const QRect> targets);

// The corner or edge of selection under point, if any.
std::optional<Grab> grabAt(const QRect& selection, QPoint point);

// selection with the grabbed edges moved to point; it flips past the far side.
QRect resized(const QRect& selection, Grab grab, QPoint point);

// selection in the frozen image's device pixels, kept inside image_rect. Edges round outward,
// so a fractional ratio (1.25, 1.5) never shaves off a row the user saw inside the frame.
QRect deviceRect(const QRect& selection, double dpr, const QRect& imageRect);

// Top-left of a size toolbar: centred under selection, all on bounds. It flips above the
// selection when there is no room below, and sits inside the selection's bottom edge when the
// selection fills the screen's height.
QPoint placeToolbar(QSize size, const QRect& selection, const QRect& bounds, int gap = kToolbarGap,
                    int margin = kEdgeMargin);

// The size readout: just above the selection's top-left, else just inside.
QRect badgeRect(QSize size, const QRect& selection, const QRect& bounds, int gap = 6);

} // namespace fv::ui_snap
