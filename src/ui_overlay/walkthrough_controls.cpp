#include "ui_overlay/walkthrough_controls.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include "ui_overlay/moments_painter.h"
#include "ui_theme/palette.h"
#include "uimath/overlay_layout.h"

namespace fv {
namespace {

void drawCamera(QPainter& painter, const QPointF& center, const QColor& color) {
    const QRectF body(center.x() - 6, center.y() - 3.2, 12, 8);
    QPainterPath bump(QPointF(center.x() - 2.6, body.top()));
    bump.lineTo(center.x() - 1.6, body.top() - 1.8);
    bump.lineTo(center.x() + 1.6, body.top() - 1.8);
    bump.lineTo(center.x() + 2.6, body.top());
    painter.setBrush(Qt::NoBrush);
    painter.setPen(roundPen(color, 1.3));
    painter.drawRoundedRect(body, 2.0, 2.0);
    painter.drawPath(bump);
    painter.drawEllipse(QPointF(center.x(), body.center().y()), 2.2, 2.2);
}

void drawCheck(QPainter& painter, const QPointF& center, const QColor& color) {
    QPainterPath tick(QPointF(center.x() - 4.0, center.y() + 0.2));
    tick.lineTo(center.x() - 1.2, center.y() + 3.0);
    tick.lineTo(center.x() + 4.2, center.y() - 2.8);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(roundPen(color, 1.6));
    painter.drawPath(tick);
}

} // namespace

using ui::tokenColor;

QString walkthroughControlKey(WalkthroughControl control) {
    return control == WalkthroughControl::Snap ? QStringLiteral("snap") : QStringLiteral("finish");
}

QString walkthroughControlLabel(WalkthroughControl control) {
    return control == WalkthroughControl::Snap ? QStringLiteral("Snap") : QStringLiteral("Done");
}

QFont walkthroughControlFont() {
    return QFont(QStringLiteral("Segoe UI"), 8, QFont::DemiBold);
}

double walkthroughControlWidth(bool labelled, int labelWidth) {
    if (!labelled) {
        return static_cast<double>(kBadgeHeight + 8);
    }
    return kControlIcon + 16 + 4 + labelWidth;
}

std::vector<PlacedControl> layoutWalkthroughControls(const WalkthroughControlsInput& in) {
    double right = in.width - kBadgeMargin;
    if (in.chipVisible) {
        right -= in.chipWidth + kControlGap;
    }
    const double left = kBadgeMargin + (in.badgeShown ? in.badgeWidth + kControlGap : 0.0);
    const double labelledWidth = walkthroughControlWidth(true, in.snapLabelWidth) +
                                 walkthroughControlWidth(true, in.doneLabelWidth) + kControlGap;
    const bool labelled = labelledWidth <= right - left;
    std::vector<PlacedControl> placed;
    // Right to left: Done sits nearest the chip.
    for (const WalkthroughControl control :
         {WalkthroughControl::Finish, WalkthroughControl::Snap}) {
        const int labelWidth =
            control == WalkthroughControl::Snap ? in.snapLabelWidth : in.doneLabelWidth;
        const double width = walkthroughControlWidth(labelled, labelWidth);
        placed.push_back(PlacedControl{control,
                                       QRectF(right - width, kBadgeMargin, width, kBadgeHeight),
                                       labelled ? walkthroughControlLabel(control) : QString()});
        right -= width + kControlGap;
    }
    return placed;
}

std::vector<PlacedControl> layoutWalkthroughControls(int width, bool chipVisible, double chipWidth,
                                                     bool badgeShown, double badgeWidth) {
    const QFontMetrics metrics(walkthroughControlFont());
    WalkthroughControlsInput in;
    in.width = width;
    in.chipVisible = chipVisible;
    in.chipWidth = chipWidth;
    in.badgeShown = badgeShown;
    in.badgeWidth = badgeWidth;
    in.snapLabelWidth =
        metrics.horizontalAdvance(walkthroughControlLabel(WalkthroughControl::Snap));
    in.doneLabelWidth =
        metrics.horizontalAdvance(walkthroughControlLabel(WalkthroughControl::Finish));
    return layoutWalkthroughControls(in);
}

void drawWalkthroughControl(QPainter& painter, const PlacedControl& placed, bool hover,
                            double radius) {
    const QRectF& rect = placed.rect;
    const bool primary = placed.control == WalkthroughControl::Snap;
    painter.setPen(Qt::NoPen);
    painter.setBrush(tokenColor("overlay-bg"));
    painter.drawRoundedRect(rect, radius, radius);
    if (primary) {
        painter.setBrush(tokenColor("accent", hover ? 255 : 215));
        painter.drawRoundedRect(rect, radius, radius);
    } else {
        painter.setBrush(tokenColor("overlay-rgb", hover ? 38 : 22));
        painter.drawRoundedRect(rect, radius, radius);
        const QColor edge = hover ? tokenColor("accent", 220) : tokenColor("overlay-border");
        painter.setPen(QPen(edge, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }
    const QColor ink = primary ? tokenColor("on-accent") : tokenColor("overlay-text");
    const bool labelled = !placed.label.isEmpty();
    const QPointF icon =
        labelled ? QPointF(rect.left() + 8 + kControlIcon / 2.0, rect.center().y()) : rect.center();
    if (primary) {
        drawCamera(painter, icon, ink);
    } else {
        drawCheck(painter, icon, ink);
    }
    if (labelled) {
        painter.setPen(QPen(ink));
        painter.setFont(walkthroughControlFont());
        const double textLeft = icon.x() + kControlIcon / 2.0 + 4;
        painter.drawText(QRectF(textLeft, rect.top(), rect.right() - textLeft, rect.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, placed.label);
    }
}

} // namespace fv
