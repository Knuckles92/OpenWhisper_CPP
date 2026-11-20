#include "ui_overlay/tidy_lines_painter.h"

#include <cmath>

#include <QColor>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QRectF>

#include "ui_theme/palette.h"
#include "uimath/color.h"
#include "uimath/overlay_look.h"
#include "uimath/tidy_lines.h"

namespace fv {
namespace {

// Python's float %: the result takes the divisor's sign.
double pyMod(double value, double modulus) {
    double result = std::fmod(value, modulus);
    if (result != 0.0 && ((result < 0.0) != (modulus < 0.0))) {
        result += modulus;
    }
    return result;
}

QColor mix(const QColor& a, const QColor& b, double f) {
    return QColor(static_cast<int>(a.red() + (b.red() - a.red()) * f),
                  static_cast<int>(a.green() + (b.green() - a.green()) * f),
                  static_cast<int>(a.blue() + (b.blue() - a.blue()) * f),
                  static_cast<int>(a.alpha() + (b.alpha() - a.alpha()) * f));
}

} // namespace

void drawTidyLines(QPainter& painter, double cx, double cy, double now) {
    const double appear = smooth((now - 0.1) / kTidyAppearS);
    if (appear <= 0.0) {
        return;
    }
    const double cyc = pyMod(now, kTidyPeriodS);
    const double left = cx - kTidyHalfSpan;
    const double caret = left - 10 + (2 * kTidyHalfSpan + 20) * smooth(cyc / kTidyCombS);
    const double loosen = smooth((cyc - kTidyLoosenAtS) / kTidyLoosenS);
    const QColor purple = ui::tokenColor("purple", static_cast<int>(255 * appear));
    painter.setPen(Qt::NoPen);
    int row = 0;
    for (const ScribbleLine& words : scribble_lines()) {
        const double y = cy - kTidyLinePitch + kTidyLinePitch * row;
        ++row;
        double x = left;
        for (const ScribbleWord& word : words) {
            const double combed = smooth((caret - (x + word.length)) / 14) * (1.0 - loosen);
            const double loose = 1.0 - combed;
            const QColor ink =
                ui::tokenColor("overlay-text", static_cast<int>((70 + 120 * combed) * appear));
            const double lit =
                detail::py_max(0.0, 1.0 - std::abs(caret - (x + word.length / 2)) / 18) *
                (1.0 - loosen);
            painter.setBrush(mix(ink, purple, lit));
            const double height = kTidyStroke + (combed <= 0.5 ? 0.8 * loose : 0.0);
            painter.drawRoundedRect(
                QRectF(x + loose * word.dx,
                       y + loose * (word.dy + 1.2 * std::sin(now * 5 + word.phase)) - height / 2,
                       word.length * (0.85 + 0.15 * combed), height),
                kTidyStroke / 2, kTidyStroke / 2);
            x += word.length + kTidyWordGap;
        }
    }
    if (cyc < kTidyCombS + 0.1) {
        const int alpha =
            static_cast<int>(230 * appear * (1.0 - smooth((cyc - kTidyCombS + 0.05) / 0.15)));
        painter.setPen(QPen(ui::tokenColor("purple", alpha), 1.8, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(caret, cy - 15), QPointF(caret, cy + 15));
    }
}

} // namespace fv
