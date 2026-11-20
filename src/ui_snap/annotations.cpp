#include "ui_snap/annotations.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPathStroker>
#include <QPen>
#include <QSizeF>

#include "pyport/number.h"

namespace fv::ui_snap {
namespace {

// A soft drop shadow lifts marks off busy screenshots without an outline.
const QColor kShadow(0, 0, 0, 70);
const QPointF kShadowOffset(0.0, 1.2);

double distance(QPointF a, QPointF b) {
    return std::hypot(b.x() - a.x(), b.y() - a.y());
}

QPen roundPen(const QColor& color, double width) {
    return QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

void fillWithShadow(QPainter& painter, const QPainterPath& path, const QColor& color) {
    painter.fillPath(path.translated(kShadowOffset), kShadow);
    painter.fillPath(path, color);
}

// Which way an arrow along (ux, uy) bows: up, or left when vertical. Hand-drawn arrows arc the
// same way whichever way they point, so the curve never flips belly-down just because the
// arrow points left.
std::pair<double, double> bulge(double ux, double uy) {
    const double nx = uy;
    const double ny = -ux;
    if (ny > 0.2 || (std::abs(ny) <= 0.2 && nx > 0)) {
        return {-nx, -ny};
    }
    return {nx, ny};
}

QFont appFont() {
    return grayscale(QGuiApplication::font());
}

bool isBoxLike(const Annotation& item) {
    return item.kind == AnnotationKind::Box || item.kind == AnnotationKind::Ellipse;
}

QPainterPath boxOutline(const Annotation& item) {
    const QRectF rect = annotationRect(item);
    QPainterPath outline;
    if (item.kind == AnnotationKind::Ellipse) {
        outline.addEllipse(rect);
        return outline;
    }
    const double radius = std::min({kBoxRadius, rect.width() / 2, rect.height() / 2});
    outline.addRoundedRect(rect, radius, radius);
    return outline;
}

void paintBox(QPainter& painter, const Annotation& item) {
    const QPainterPath outline = boxOutline(item);
    if (item.infill) {
        QColor fill(item.color);
        fill.setAlpha(kBoxFillAlpha);
        painter.fillPath(outline, fill);
    }
    painter.strokePath(outline.translated(kShadowOffset), roundPen(kShadow, item.width));
    painter.strokePath(outline, roundPen(item.color, item.width));
}

void paintStroke(QPainter& painter, const Annotation& item) {
    QPainterPath path = smoothPath(
        std::span<const QPointF>(item.points.constData(), std::size_t(item.points.size())));
    if (item.points.size() == 1) {
        // A click with the pen leaves a dot, as a real pen would.
        path.lineTo(item.points.front() + QPointF(0.01, 0));
    }
    const double width = strokeWidth(item);
    if (item.highlighter) {
        QColor color(item.color);
        color.setAlpha(kHighlighterAlpha);
        // One stroke call covers each pixel once, so crossings don't darken.
        painter.strokePath(path, roundPen(color, width));
        return;
    }
    painter.strokePath(path.translated(kShadowOffset), roundPen(kShadow, width));
    painter.strokePath(path, roundPen(item.color, width));
}

void paintLabel(QPainter& painter, const Annotation& item) {
    const QRectF rect = annotationRect(item);
    QPainterPath tag;
    tag.addRoundedRect(rect, kLabelRadius, kLabelRadius);
    fillWithShadow(painter, tag, item.color);
    painter.setFont(grayscale(item.font));
    painter.setPen(inkFor(item.color));
    painter.drawText(rect.adjusted(kLabelPadX, 0, -kLabelPadX / 2, 0),
                     Qt::AlignLeft | Qt::AlignVCenter, item.text);
}

void paintStep(QPainter& painter, const Annotation& item) {
    const double ring = kStepRadius + kStepRing;
    painter.setPen(Qt::NoPen);
    painter.setBrush(kShadow);
    painter.drawEllipse(item.center + kShadowOffset, ring, ring);
    painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
    painter.drawEllipse(item.center, ring, ring);
    painter.setBrush(item.color);
    painter.drawEllipse(item.center, kStepRadius, kStepRadius);
    painter.setFont(stepFont(item.number));
    painter.setPen(inkFor(item.color));
    const QRectF box(item.center.x() - kStepRadius, item.center.y() - kStepRadius, 2 * kStepRadius,
                     2 * kStepRadius);
    painter.drawText(box, Qt::AlignCenter, QString::number(item.number));
}

} // namespace

const char* toolName(SnapTool tool) {
    switch (tool) {
    case SnapTool::Arrow:
        return "arrow";
    case SnapTool::Box:
        return "box";
    case SnapTool::Ellipse:
        return "ellipse";
    case SnapTool::Pen:
        return "pen";
    case SnapTool::Highlighter:
        return "highlighter";
    case SnapTool::Text:
        return "text";
    case SnapTool::Step:
        return "step";
    case SnapTool::Redact:
        return "redact";
    }
    return "";
}

QColor inkFor(const QColor& color) {
    const auto linear = [](double channel) {
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    const double luminance = 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) +
                             0.0722 * linear(color.blueF());
    return luminance < 0.3 ? QColor(QStringLiteral("#FFFFFF")) : QColor(QStringLiteral("#2A1A06"));
}

QFont grayscale(const QFont& font) {
    QFont plain(font);
    plain.setStyleStrategy(
        QFont::StyleStrategy(QFont::PreferAntialias | QFont::NoSubpixelAntialias));
    return plain;
}

QFont labelFont(int pixelSize) {
    QFont font = appFont();
    font.setPixelSize(pixelSize);
    font.setWeight(QFont::DemiBold);
    return font;
}

QFont stepFont(int number) {
    QFont font = appFont();
    font.setPixelSize(number < 100 ? 14 : 11);
    font.setWeight(QFont::Bold);
    return font;
}

std::pair<QPainterPath, QPolygonF> arrowShapes(QPointF start, QPointF end, double width) {
    const double dx = end.x() - start.x();
    const double dy = end.y() - start.y();
    const double length = std::hypot(dx, dy);
    if (length < 1e-6) {
        return {QPainterPath(start), QPolygonF()};
    }
    const auto [nx, ny] = bulge(dx / length, dy / length);
    const double bend = std::min(length * kArrowBend, kArrowMaxBend);
    const QPointF control((start.x() + end.x()) / 2 + nx * bend,
                          (start.y() + end.y()) / 2 + ny * bend);
    const QPointF tangent = end - control;
    double reach = std::hypot(tangent.x(), tangent.y());
    if (reach == 0.0) {
        reach = 1.0;
    }
    const double ux = tangent.x() / reach;
    const double uy = tangent.y() / reach;
    const double head = std::min(4.0 + width * 3.2, length * 0.6);
    const double half = head * 0.62;
    const QPointF base(end.x() - ux * head, end.y() - uy * head);
    const QPointF notch(end.x() - ux * head * 0.78, end.y() - uy * head * 0.78);
    const QPointF left(base.x() - uy * half, base.y() + ux * half);
    const QPointF right(base.x() + uy * half, base.y() - ux * half);
    QPainterPath shaft(start);
    shaft.quadTo(control, QPointF(end.x() - ux * head * 0.6, end.y() - uy * head * 0.6));
    return {shaft, QPolygonF({end, left, notch, right})};
}

QPainterPath arrowOutline(QPointF start, QPointF end, double width) {
    const auto [shaft, head] = arrowShapes(start, end, width);
    QPainterPathStroker stroker;
    stroker.setWidth(width);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    const QPainterPath outline = stroker.createStroke(shaft);
    if (head.isEmpty()) {
        return outline;
    }
    QPainterPath tip;
    tip.addPolygon(head);
    tip.closeSubpath();
    return outline.united(tip);
}

QPainterPath smoothPath(std::span<const QPointF> points) {
    QPainterPath path(points.front());
    for (std::size_t i = 1; i + 1 < points.size(); ++i) {
        const QPointF current = points[i];
        const QPointF following = points[i + 1];
        path.quadTo(current, (current + following) / 2);
    }
    if (points.size() > 1) {
        path.lineTo(points.back());
    }
    return path;
}

Annotation Annotation::arrow(QPointF start, QPointF end, const QColor& color, double width) {
    Annotation item;
    item.kind = AnnotationKind::Arrow;
    item.start = start;
    item.end = end;
    item.color = color;
    item.width = width;
    return item;
}

Annotation Annotation::box(QPointF start, QPointF end, const QColor& color, double width,
                           bool infill) {
    Annotation item = arrow(start, end, color, width);
    item.kind = AnnotationKind::Box;
    item.infill = infill;
    return item;
}

Annotation Annotation::ellipse(QPointF start, QPointF end, const QColor& color, double width,
                               bool infill) {
    Annotation item = box(start, end, color, width, infill);
    item.kind = AnnotationKind::Ellipse;
    return item;
}

Annotation Annotation::redaction(QPointF start, QPointF end) {
    Annotation item;
    item.kind = AnnotationKind::Redaction;
    item.start = start;
    item.end = end;
    return item;
}

Annotation Annotation::stroke(QList<QPointF> points, const QColor& color, bool highlighter) {
    Annotation item;
    item.kind = AnnotationKind::Stroke;
    item.points = std::move(points);
    item.color = color;
    item.highlighter = highlighter;
    return item;
}

Annotation Annotation::label(QPointF pos, QString text, const QColor& color) {
    return label(pos, std::move(text), color, labelFont());
}

Annotation Annotation::label(QPointF pos, QString text, const QColor& color, const QFont& font) {
    Annotation item;
    item.kind = AnnotationKind::Label;
    item.pos = pos;
    item.text = std::move(text);
    item.color = color;
    item.font = font;
    return item;
}

Annotation Annotation::step(QPointF center, int number, const QColor& color) {
    Annotation item;
    item.kind = AnnotationKind::Step;
    item.center = center;
    item.number = number;
    item.color = color;
    return item;
}

double strokeWidth(const Annotation& stroke) {
    return stroke.highlighter ? kHighlighterWidth : kPenWidth;
}

QRectF annotationRect(const Annotation& item) {
    if (item.kind == AnnotationKind::Label) {
        const QFontMetricsF metrics(item.font);
        const QSizeF size(metrics.horizontalAdvance(item.text) + 2 * kLabelPadX,
                          metrics.height() + 2 * kLabelPadY);
        return QRectF(item.pos, size);
    }
    return QRectF(item.start, item.end).normalized();
}

void extendAnnotation(Annotation& item, QPointF point) {
    switch (item.kind) {
    case AnnotationKind::Arrow:
    case AnnotationKind::Box:
    case AnnotationKind::Ellipse:
    case AnnotationKind::Redaction:
        item.end = point;
        return;
    case AnnotationKind::Stroke:
        // Sub-pixel jitter only adds bumps to the smoothed curve.
        if (distance(item.points.back(), point) >= 1.5) {
            item.points.append(point);
        }
        return;
    case AnnotationKind::Label:
    case AnnotationKind::Step:
        return;
    }
}

bool meaningful(const Annotation& item) {
    switch (item.kind) {
    case AnnotationKind::Arrow:
        return distance(item.start, item.end) >= kMinDrag;
    case AnnotationKind::Box:
    case AnnotationKind::Ellipse: {
        const QRectF rect = annotationRect(item);
        return rect.width() >= kMinDrag / 2 && rect.height() >= kMinDrag / 2 &&
               distance(item.start, item.end) >= kMinDrag;
    }
    case AnnotationKind::Redaction: {
        const QRectF rect = annotationRect(item);
        return rect.width() >= kMinDrag / 2 && rect.height() >= kMinDrag / 2;
    }
    case AnnotationKind::Stroke:
        return !item.points.isEmpty();
    case AnnotationKind::Label:
        return !item.text.trimmed().isEmpty();
    case AnnotationKind::Step:
        return true;
    }
    return false;
}

QRectF annotationBounds(const Annotation& item) {
    switch (item.kind) {
    case AnnotationKind::Arrow: {
        // From the parts, not the united outline: this runs on every drag step.
        const auto [shaft, head] = arrowShapes(item.start, item.end, item.width);
        const double grow = item.width / 2 + 3;
        return shaft.boundingRect()
            .united(head.boundingRect())
            .adjusted(-grow, -grow, grow, grow + 1);
    }
    case AnnotationKind::Box:
    case AnnotationKind::Ellipse: {
        const double grow = item.width + 3;
        return annotationRect(item).adjusted(-grow, -grow, grow, grow + 1);
    }
    case AnnotationKind::Redaction:
        // Cover the boundary pixels too, including on fractional-scale screens.
        return QRectF(annotationRect(item).toAlignedRect().adjusted(-1, -1, 1, 1));
    case AnnotationKind::Stroke: {
        // The smoothed curve never leaves the points' box, which is cheap to find on every
        // drag step of a long stroke.
        const double grow = strokeWidth(item) / 2 + 3;
        return QPolygonF(item.points).boundingRect().adjusted(-grow, -grow, grow, grow + 1);
    }
    case AnnotationKind::Label:
        return annotationRect(item).adjusted(-2, -2, 2, 3);
    case AnnotationKind::Step: {
        const double reach = kStepRadius + kStepRing + 3;
        return QRectF(item.center.x() - reach, item.center.y() - reach, 2 * reach, 2 * reach + 1);
    }
    }
    return {};
}

void paintAnnotation(QPainter& painter, const Annotation& item) {
    switch (item.kind) {
    case AnnotationKind::Arrow:
        if (distance(item.start, item.end) < 1.0) {
            return;
        }
        fillWithShadow(painter, arrowOutline(item.start, item.end, item.width), item.color);
        return;
    case AnnotationKind::Box:
    case AnnotationKind::Ellipse:
        paintBox(painter, item);
        return;
    case AnnotationKind::Redaction:
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.fillRect(annotationBounds(item), QColor(QString::fromLatin1(kRedactionColor)));
        painter.restore();
        return;
    case AnnotationKind::Stroke:
        paintStroke(painter, item);
        return;
    case AnnotationKind::Label:
        paintLabel(painter, item);
        return;
    case AnnotationKind::Step:
        paintStep(painter, item);
        return;
    }
}

std::optional<Annotation> newDraft(SnapTool tool, QPointF point, const QColor& color,
                                   bool boxInfill) {
    switch (tool) {
    case SnapTool::Arrow:
        return Annotation::arrow(point, point, color);
    case SnapTool::Box:
        return Annotation::box(point, point, color, kBoxWidth, boxInfill);
    case SnapTool::Ellipse:
        return Annotation::ellipse(point, point, color, kBoxWidth, boxInfill);
    case SnapTool::Redact:
        return Annotation::redaction(point, point);
    case SnapTool::Pen:
        return Annotation::stroke({point}, color);
    case SnapTool::Highlighter:
        return Annotation::stroke({point}, color, true);
    case SnapTool::Text:
    case SnapTool::Step:
        return std::nullopt;
    }
    return std::nullopt;
}

void extendDraft(Annotation& draft, QPointF point, bool constrained, std::optional<QRectF> bounds) {
    if (constrained && (draft.kind == AnnotationKind::Arrow || isBoxLike(draft))) {
        const QPointF start = draft.start;
        const double dx = point.x() - start.x();
        const double dy = point.y() - start.y();
        double ux = 0.0;
        double uy = 0.0;
        double reach = 0.0;
        if (isBoxLike(draft)) {
            ux = std::copysign(1.0, dx);
            uy = std::copysign(1.0, dy);
            reach = std::max(std::abs(dx), std::abs(dy));
        } else {
            constexpr double kBearing = std::numbers::pi / 4;
            const double angle = py::round_half_even(std::atan2(dy, dx) / kBearing) * kBearing;
            ux = std::cos(angle);
            uy = std::sin(angle);
            reach = std::hypot(dx, dy);
        }
        if (bounds) {
            const struct {
                double origin, direction, low, high;
            } axes[] = {{start.x(), ux, bounds->left(), bounds->right()},
                        {start.y(), uy, bounds->top(), bounds->bottom()}};
            for (const auto& axis : axes) {
                if (std::abs(axis.direction) > 1e-6) {
                    const double edge = axis.direction > 0 ? axis.high : axis.low;
                    reach = std::min(reach, std::max(0.0, (edge - axis.origin) / axis.direction));
                }
            }
        }
        point = start + QPointF(ux * reach, uy * reach);
    }
    extendAnnotation(draft, point);
}

void AnnotationStack::add(Annotation item) {
    items_.push_back(std::move(item));
    undone_.clear();
}

std::optional<Annotation> AnnotationStack::undo() {
    if (items_.empty()) {
        return std::nullopt;
    }
    Annotation item = std::move(items_.back());
    items_.pop_back();
    undone_.push_back(item);
    return item;
}

std::optional<Annotation> AnnotationStack::redo() {
    if (undone_.empty()) {
        return std::nullopt;
    }
    Annotation item = std::move(undone_.back());
    undone_.pop_back();
    items_.push_back(item);
    return item;
}

int AnnotationStack::nextStepNumber() const {
    return 1 + static_cast<int>(std::count_if(items_.begin(), items_.end(), [](const auto& item) {
               return item.kind == AnnotationKind::Step;
           }));
}

} // namespace fv::ui_snap
