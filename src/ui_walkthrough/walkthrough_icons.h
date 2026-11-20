#pragma once

#include <string_view>

#include <QColor>
#include <QPainterPath>
#include <QPointF>

class QPainter;

// The walkthrough column's glyphs, drawn as vector paths on a 24-unit grid
// (ui_qt/widgets/walkthrough_icons.py): drawn rather than loaded so they stay sharp at every
// scale and take any colour the column paints them in. Affinity: GUI thread.
namespace fv {

// Glyph name on its 24-unit grid: camera, up, down, x, undo, check, note, pause, play, copy.
// Any other name is an empty path.
QPainterPath walkthroughIconPath(std::string_view name);

// Draws icon name size pixels square around center. name must be one of the ten glyphs.
void paintWalkthroughIcon(QPainter& painter, std::string_view name, QPointF center, double size,
                          const QColor& color);

} // namespace fv
