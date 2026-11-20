#include "ui_snap/icons.h"

#include <array>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointF>

#include "ui_snap/annotations.h"

namespace fv::ui_snap {
namespace {

constexpr double kLine = 1.7;

QPen linePen(const QColor& color, double width = kLine) {
    return QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

QPainterPath polyline(std::initializer_list<QPointF> points) {
    QPainterPath path(*points.begin());
    for (auto point = points.begin() + 1; point != points.end(); ++point) {
        path.lineTo(*point);
    }
    return path;
}

void arrow(QPainter& painter, const QColor& color) {
    painter.fillPath(arrowOutline(QPointF(3.5, 16.5), QPointF(16.5, 3.5), 1.8), color);
}

void box(QPainter& painter, const QColor& color) {
    painter.setPen(linePen(color));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(3.2, 4.6, 13.6, 10.8), 2.6, 2.6);
}

void ellipse(QPainter& painter, const QColor& color) {
    painter.setPen(linePen(color));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QRectF(3, 4, 14, 12));
}

void redact(QPainter& painter, const QColor& color) {
    painter.setPen(linePen(color, 3.2));
    painter.drawLine(QPointF(4, 6), QPointF(16, 6));
    painter.drawLine(QPointF(4, 10), QPointF(13, 10));
    painter.drawLine(QPointF(4, 14), QPointF(16, 14));
}

void pen(QPainter& painter, const QColor& color) {
    const std::array<QPointF, 6> points{QPointF(3, 13),     QPointF(5.5, 7.5), QPointF(8.5, 12.5),
                                        QPointF(11.5, 6.5), QPointF(14, 12),   QPointF(17, 6.5)};
    painter.strokePath(smoothPath(points), linePen(color, 1.9));
}

void highlighter(QPainter& painter, const QColor& color) {
    // A marker (wide barrel, narrow chisel tip) over the swipe it leaves.
    QColor swipe(color);
    swipe.setAlphaF(0.45f);
    painter.strokePath(polyline({{3.4, 16.0}, {16.6, 16.0}}), linePen(swipe, 4.4));
    painter.strokePath(polyline({{9.4, 9.6}, {15.2, 3.8}}), linePen(color, 5.0));
    painter.strokePath(polyline({{6.6, 12.6}, {9.4, 9.6}}), linePen(color, 2.4));
}

void text(QPainter& painter, const QColor& color) {
    const QPen pen = linePen(color, 2.0);
    painter.strokePath(polyline({{4.5, 4.5}, {15.5, 4.5}}), pen);
    painter.strokePath(polyline({{10.0, 4.5}, {10.0, 16.0}}), pen);
    painter.strokePath(polyline({{7.5, 16.0}, {12.5, 16.0}}), linePen(color, 1.6));
}

void step(QPainter& painter, const QColor& color) {
    painter.setPen(linePen(color));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QPointF(10, 10), 7.2, 7.2);
    QFont font(stepFont(1));
    font.setPixelSize(10);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(QRectF(2.8, 2.6, 14.4, 14.6), Qt::AlignCenter, QStringLiteral("1"));
}

std::pair<QPainterPath, QPainterPath> undoPath(bool mirror) {
    const auto x = [mirror](double value) {
        return mirror ? kIconGrid - value : value;
    };
    QPainterPath curve(QPointF(x(15.5), 15.5));
    curve.cubicTo(QPointF(x(16.5), 9.0), QPointF(x(12.0), 6.8), QPointF(x(5.0), 7.2));
    QPainterPath head = polyline({{x(8.4), 3.8}, {x(5.0), 7.2}, {x(8.4), 10.6}});
    return {curve, head};
}

void strokeUndo(QPainter& painter, const QColor& color, bool mirror) {
    const auto [curve, head] = undoPath(mirror);
    painter.strokePath(curve, linePen(color));
    painter.strokePath(head, linePen(color));
}

void undo(QPainter& painter, const QColor& color) {
    strokeUndo(painter, color, false);
}

void redo(QPainter& painter, const QColor& color) {
    strokeUndo(painter, color, true);
}

void check(QPainter& painter, const QColor& color) {
    painter.strokePath(polyline({{4.6, 10.4}, {8.4, 14.2}, {15.6, 6.0}}), linePen(color, 2.2));
}

void cross(QPainter& painter, const QColor& color) {
    const QPen pen = linePen(color, 1.9);
    painter.strokePath(polyline({{5.6, 5.6}, {14.4, 14.4}}), pen);
    painter.strokePath(polyline({{14.4, 5.6}, {5.6, 14.4}}), pen);
}

void snap(QPainter& painter, const QColor& color) {
    const QPen pen = linePen(color);
    for (const auto [x, dx] : {std::pair{3.5, 1.0}, std::pair{16.5, -1.0}}) {
        for (const auto [y, dy] : {std::pair{3.5, 1.0}, std::pair{16.5, -1.0}}) {
            painter.strokePath(polyline({{x + 4 * dx, y}, {x, y}, {x, y + 4 * dy}}), pen);
        }
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QPointF(10, 10), 1.8, 1.8);
}

void clear(QPainter& painter, const QColor& color) {
    const QPen pen = linePen(color);
    painter.strokePath(polyline({{4, 5.5}, {16, 5.5}}), pen);
    painter.strokePath(polyline({{7.5, 5.5}, {8, 3}, {12, 3}, {12.5, 5.5}}), pen);
    painter.strokePath(polyline({{5.5, 5.5}, {7, 17}, {13, 17}, {14.5, 5.5}}), pen);
}

using Glyph = void (*)(QPainter&, const QColor&);

constexpr std::array<std::string_view, 15> kNames{
    "arrow", "box",  "ellipse", "redact", "pen",  "highlighter", "text",  "step",
    "undo",  "redo", "keep",    "drop",   "snap", "region",      "clear",
};
constexpr std::array<Glyph, 15> kGlyphs{
    arrow, box,  ellipse, redact, pen,  highlighter, text,  step,
    undo,  redo, check,   cross,  snap, snap,        clear,
};

} // namespace

std::span<const std::string_view> snapIconNames() {
    return kNames;
}

void paintSnapIcon(QPainter& painter, std::string_view name, const QRectF& rect,
                   const QColor& color) {
    Glyph glyph = nullptr;
    for (std::size_t index = 0; index < kNames.size(); ++index) {
        if (kNames[index] == name) {
            glyph = kGlyphs[index];
        }
    }
    if (glyph == nullptr) {
        throw std::out_of_range("no snap icon " + std::string(name));
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(rect.topLeft());
    painter.scale(rect.width() / kIconGrid, rect.height() / kIconGrid);
    glyph(painter, color);
    painter.restore();
}

} // namespace fv::ui_snap
