#pragma once

#include <span>
#include <string_view>

#include <QColor>
#include <QRectF>

class QPainter;

// The Snap toolbar's glyphs, drawn as vector paths (ui_qt/snap/icons.py): sharp at every scale,
// and the arrow and step icons are tiny versions of the marks they make. Each glyph is designed
// on a 20 x 20 grid and scaled into its rect.
namespace fv::ui_snap {

inline constexpr double kIconGrid = 20.0;

// The glyph names: arrow, box, ellipse, redact, pen, highlighter, text, step, undo, redo, keep
// (a check), drop (a cross), snap, region (the snap glyph) and clear.
std::span<const std::string_view> snapIconNames();

// Draws glyph name scaled into rect in color. Throws std::out_of_range for an unknown name.
void paintSnapIcon(QPainter& painter, std::string_view name, const QRectF& rect,
                   const QColor& color);

} // namespace fv::ui_snap
