#pragma once

#include <string_view>

#include <QPointF>
#include <QRectF>
#include <QString>

class QPainter;

namespace fv {

// The live recording label (ui_qt/overlays/live_recording.py): "● Recording 0:04", each part
// optional and the rest kept centred. The dot pops in with a ring, then pulses; the words rise
// into place. elapsed is seconds since the recording started; tones are palette tokens.
// Affinity: GUI thread.

// How wide drawLiveLabel draws "● 0:04" with text off.
double wordlessLabelWidth(double elapsed, bool dot = true, bool clock = true);

void drawLiveLabel(QPainter& painter, const QRectF& rect, const QString& label, double elapsed,
                   std::string_view dotTone, std::string_view textTone = "overlay-text",
                   bool dot = true, bool text = true, bool clock = true);

void drawLiveDot(QPainter& painter, const QPointF& center, double elapsed, std::string_view tone,
                 double diameter);

} // namespace fv
