#include "ui_walkthrough/walkthrough_icons.h"

#include <array>
#include <initializer_list>

#include <QPainter>
#include <QPen>
#include <QRectF>

namespace fv {
namespace {

struct Stroke {
    std::string_view name;
    double width; // grid units; 0 draws no outline
    bool filled;
};

constexpr std::array<Stroke, 10> kStrokes{{
    {"camera", 2.0, false},
    {"up", 2.4, false},
    {"down", 2.4, false},
    {"x", 2.4, false},
    {"undo", 2.0, false},
    {"check", 2.8, false},
    {"note", 2.2, false},
    {"pause", 0.0, true},
    {"play", 1.5, true},
    {"copy", 2.0, false},
}};

void line(QPainterPath& path, std::initializer_list<QPointF> points) {
    auto point = points.begin();
    path.moveTo(*point);
    for (++point; point != points.end(); ++point) {
        path.lineTo(*point);
    }
}

} // namespace

QPainterPath walkthroughIconPath(std::string_view name) {
    QPainterPath path;
    if (name == "camera") {
        line(path, {{4, 8}, {7, 8}, {9, 5}, {15, 5}, {17, 8}, {20, 8}, {20, 19}, {4, 19}, {4, 8}});
        path.addEllipse(QPointF(12, 13), 3.5, 3.5);
    } else if (name == "up") {
        line(path, {{6, 15}, {12, 9}, {18, 15}});
    } else if (name == "down") {
        line(path, {{6, 9}, {12, 15}, {18, 9}});
    } else if (name == "x") {
        line(path, {{6, 6}, {18, 18}});
        line(path, {{18, 6}, {6, 18}});
    } else if (name == "undo") {
        line(path, {{9, 14}, {4, 9}, {9, 4}});
        path.moveTo(4, 9);
        path.lineTo(14, 9);
        path.arcTo(QRectF(8, 9, 12, 12), 90, -180);
        path.lineTo(11, 21);
    } else if (name == "check") {
        line(path, {{5, 12.5}, {9.5, 17}, {19, 7.5}});
    } else if (name == "note") {
        line(path, {{5, 4}, {19, 4}, {19, 16}, {9, 16}, {5, 20}, {5, 4}});
    } else if (name == "pause") {
        path.addRoundedRect(QRectF(6, 5, 4, 14), 1, 1);
        path.addRoundedRect(QRectF(14, 5, 4, 14), 1, 1);
    } else if (name == "play") {
        line(path, {{8, 5}, {19, 12}, {8, 19}, {8, 5}});
    } else if (name == "copy") {
        path.addRoundedRect(QRectF(8, 8, 12, 12), 2.5, 2.5);
        line(path, {{5, 16}, {4, 16}, {4, 6}});
        path.arcTo(QRectF(4, 4, 4, 4), 180, -90);
        path.lineTo(16, 4);
    }
    return path;
}

void paintWalkthroughIcon(QPainter& painter, std::string_view name, QPointF center, double size,
                          const QColor& color) {
    const Stroke* stroke = nullptr;
    for (const Stroke& entry : kStrokes) {
        if (entry.name == name) {
            stroke = &entry;
        }
    }
    if (stroke == nullptr) {
        return;
    }
    painter.save();
    painter.translate(center.x() - size / 2, center.y() - size / 2);
    painter.scale(size / 24, size / 24);
    if (stroke->filled) {
        painter.setBrush(color);
    } else {
        painter.setBrush(Qt::NoBrush);
    }
    if (stroke->width != 0.0) {
        painter.setPen(QPen(color, stroke->width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    } else {
        painter.setPen(Qt::NoPen);
    }
    painter.drawPath(walkthroughIconPath(name));
    painter.restore();
}

} // namespace fv
