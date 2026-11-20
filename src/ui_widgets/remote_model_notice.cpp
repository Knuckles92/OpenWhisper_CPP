#include "ui_widgets/remote_model_notice.h"

#include <QPainter>
#include <QPen>
#include <QSizePolicy>

#include "ui_theme/palette.h"

namespace fv {
namespace {

// Tabler's download icon, on its 24-unit grid.
QPainterPath downloadPath() {
    QPainterPath path(QPointF(4, 17));
    path.lineTo(4, 19);
    path.arcTo(QRectF(4, 17, 4, 4), 180, 90);
    path.lineTo(18, 21);
    path.arcTo(QRectF(16, 17, 4, 4), 270, 90);
    path.lineTo(20, 17);
    path.moveTo(7, 11);
    path.lineTo(12, 16);
    path.lineTo(17, 11);
    path.moveTo(12, 4);
    path.lineTo(12, 16);
    return path;
}

} // namespace

RemoteModelNotice::RemoteModelNotice(QWidget* parent)
    : QPushButton(QStringLiteral("Manage host models"), parent), glyph_(downloadPath()) {
    setObjectName(QStringLiteral("remoteModelNotice"));
    setFlat(true);
    setCursor(Qt::PointingHandCursor);
    // A click shouldn't leave the link underlined; Tab still reaches it.
    setFocusPolicy(Qt::TabFocus);
    // Takes the line's height rather than setting it: a push button's native minimum is taller
    // than the footer's text.
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    setAvailable({});
}

void RemoteModelNotice::setAvailable(const QStringList& names) {
    available_ = names;
    QString tip = QStringLiteral(
        "Browse models, manage downloads, and set up CPU or GPU on the paired host.");
    if (!names.isEmpty()) {
        tip = QStringLiteral("Available to install on the paired host: %1. %2")
                  .arg(names.join(QStringLiteral(", ")), tip);
    }
    setToolTip(tip);
    setAccessibleDescription(tip);
    update();
}

void RemoteModelNotice::paintEvent(QPaintEvent* event) {
    QPushButton::paintEvent(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(0, (height() - kGlyphSize) / 2.0);
    painter.scale(kGlyphSize / 24.0, kGlyphSize / 24.0);
    // The text's colour, hover included, so glyph and words read as one link.
    const bool hovered = underMouse() && !isDown();
    QPen pen(ui::tokenColor(hovered ? "accent-soft" : "accent"), 2);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.drawPath(glyph_);
    if (!available_.isEmpty()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(ui::tokenColor("warning"));
        painter.drawEllipse(QPointF(20.5, 4), 3.5, 3.5);
    }
}

} // namespace fv
