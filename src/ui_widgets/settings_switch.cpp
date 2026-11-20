#include "ui_widgets/settings_switch.h"

#include <QEvent>
#include <QPainter>
#include <QPen>
#include <QSizePolicy>

#include "ui_theme/font_scale.h"
#include "ui_theme/theme.h"

namespace fv {

SettingsSwitch::SettingsSwitch(QWidget* parent) : QAbstractButton(parent) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setObjectName(QStringLiteral("settingsSwitch"));
}

QSize SettingsSwitch::sizeHint() const {
    return QSize(ui::scaled(46), ui::scaled(28));
}

void SettingsSwitch::changeEvent(QEvent* event) {
    QAbstractButton::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        updateGeometry();
        update();
    }
}

void SettingsSwitch::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled()) {
        painter.setOpacity(0.45);
    }
    const ui::Theme& theme = ui::currentTheme();
    const QRectF rect = QRectF(this->rect()).adjusted(2, 2, -2, -2);
    painter.setPen(Qt::NoPen);
    // A disabled switch keeps its knob position but never shows the accent track, so a feature
    // that can't run doesn't read as "on".
    const bool onTrack = isChecked() && isEnabled();
    painter.setBrush(onTrack ? theme.accent : theme.slateBorderStrong);
    painter.drawRoundedRect(rect, rect.height() / 2, rect.height() / 2);
    const double diameter = rect.height() - 6;
    const double x = isChecked() ? rect.right() - diameter - 3 : rect.left() + 3;
    painter.setBrush(theme.onAccent);
    painter.drawEllipse(QRectF(x, rect.top() + 3, diameter, diameter));
    if (hasFocus()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(theme.accent, 1));
        painter.drawRoundedRect(QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5), 14, 14);
    }
}

} // namespace fv
