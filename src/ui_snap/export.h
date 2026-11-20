#pragma once

#include <span>

#include <QImage>
#include <QRect>

#include "ui_snap/annotations.h"

// Turning the frozen screen, a selection and its annotations into the kept image
// (ui_qt/snap/export.py).
namespace fv::ui_snap {

// The whole image in logical pixels: what "the whole screen" selects.
QRect logicalRect(const QImage& image);

// selection of frozen with items drawn on, in device pixels. The crop is taken straight from
// the captured pixels and the annotations are drawn with the painter scaled by the
// device-pixel ratio and clipped to the selection, so a 2x screen keeps 2x detail and strokes
// stay crisp rather than upscaled. The result carries the screen's ratio: size() is device
// pixels and deviceIndependentSize() the size it had on screen. frozen is not changed.
QImage renderKept(const QImage& frozen, const QRect& selection,
                  std::span<const Annotation> items = {});

} // namespace fv::ui_snap
