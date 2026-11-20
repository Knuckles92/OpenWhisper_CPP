#include "ui_overlay/live_label_painter.h"

#include <cmath>
#include <vector>

#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPen>

#include "ui_overlay/moments_painter.h"
#include "ui_theme/palette.h"
#include "uimath/easing.h"
#include "uimath/live_label.h"
#include "uimath/moments.h"

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

QFont labelFont() {
    return QFont(QStringLiteral("Segoe UI"), 10, QFont::Bold);
}

QFont clockFont() {
    return QFont(QStringLiteral("Segoe UI"), 10);
}

enum class Part { Dot, Text, Clock };

} // namespace

double wordlessLabelWidth(double elapsed, bool dot, bool clock) {
    std::vector<double> parts;
    if (dot) {
        parts.push_back(kLiveDotDiameter);
    }
    if (clock) {
        parts.push_back(QFontMetrics(clockFont())
                            .horizontalAdvance(QString::fromStdString(format_clock(elapsed))));
    }
    double sum = 0.0;
    for (const double part : parts) {
        sum += part;
    }
    const auto gaps = parts.empty() ? 0 : static_cast<int>(parts.size()) - 1;
    return sum + kLiveLabelGap * gaps;
}

void drawLiveLabel(QPainter& painter, const QRectF& rect, const QString& label, double elapsed,
                   std::string_view dotTone, std::string_view textTone, bool dot, bool text,
                   bool clock) {
    const QFont font = labelFont();
    const QFont timeFont = clockFont();
    const QFontMetrics metrics(font);
    const QFontMetrics clockMetrics(timeFont);
    const double dotD = kLiveDotDiameter;
    const double gap = kLiveLabelGap;
    const QString clockText = clock ? QString::fromStdString(format_clock(elapsed)) : QString();
    const double clockW = clock ? clockMetrics.horizontalAdvance(clockText) : 0.0;
    // Each part shown, with its width; the gaps go between them.
    std::vector<std::pair<Part, double>> parts;
    QString shown = label;
    if (dot) {
        parts.emplace_back(Part::Dot, dotD);
    }
    if (text) {
        double reserved = 0.0;
        if (dot) {
            reserved += dotD + gap;
        }
        if (clock) {
            reserved += clockMetrics.horizontalAdvance(QStringLiteral("00:00")) + gap;
        }
        const double room = rect.width() - reserved;
        shown = metrics.elidedText(label, Qt::ElideRight, static_cast<int>(room > 0 ? room : 0.0));
        parts.emplace_back(Part::Text, metrics.horizontalAdvance(shown));
    }
    if (clock) {
        parts.emplace_back(Part::Clock, clockW);
    }
    if (parts.empty()) {
        return;
    }
    double width = 0.0;
    for (const auto& [kind, partW] : parts) {
        width += partW;
    }
    width += gap * static_cast<double>(parts.size() - 1);
    double x = rect.center().x() - width / 2;
    const double rise = ease_out_cubic(phase(elapsed, 0.06, kLabelS));
    const double top = rect.top() + 6.0 * (1.0 - rise);
    const int flags = Qt::AlignLeft | Qt::AlignVCenter;
    for (const auto& [kind, partW] : parts) {
        if (kind == Part::Dot) {
            drawLiveDot(painter, QPointF(x + dotD / 2, rect.center().y() + 1), elapsed, dotTone,
                        dotD);
        } else if (kind == Part::Text) {
            painter.setFont(font);
            painter.setPen(QPen(ui::tokenColor(textTone, static_cast<int>(255 * rise))));
            painter.drawText(QRectF(x, top, partW + 2, rect.height()), flags, shown);
        } else {
            painter.setFont(timeFont);
            // On its own the clock reads at full strength; beside the words it recedes.
            const int clockAlpha = text ? 150 : 230;
            painter.setPen(QPen(ui::tokenColor(textTone, static_cast<int>(clockAlpha * rise))));
            painter.drawText(QRectF(x, top, partW + 2, rect.height()), flags, clockText);
        }
        x += partW + gap;
    }
}

void drawLiveDot(QPainter& painter, const QPointF& center, double elapsed, std::string_view tone,
                 double diameter) {
    drawRings(painter, center, elapsed, 0.0, {tone}, 10.0, diameter / 2);
    const double pop = phase(elapsed, 0.0, kPopS);
    if (pop <= 0.0) {
        return;
    }
    const double settle = kPopS + 0.4;
    painter.setPen(Qt::NoPen);
    if (elapsed > settle) {
        const double pulse = pyMod(elapsed - settle, kLivePulseS) / kLivePulseS;
        const double radius = diameter / 2 + 6.0 * (1.0 - std::pow(1.0 - pulse, 3));
        painter.setBrush(ui::tokenColor(tone, static_cast<int>(110 * (1.0 - pulse))));
        painter.drawEllipse(center, radius, radius);
    }
    painter.setBrush(ui::tokenColor(tone));
    const double radius = diameter / 2 * ease_out_back(pop);
    painter.drawEllipse(center, radius, radius);
}

} // namespace fv
