#include "ui_widgets/buttons.h"

#include <algorithm>

#include <QEasingCurve>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QPropertyAnimation>
#include <QSizePolicy>

#include "ui_theme/local_style.h"
#include "ui_theme/style_props.h"
#include "ui_theme/theme.h"
#include "ui_widgets/collapse_animation.h"

namespace fv {
namespace {

// Applied to the pill's label (ui_qt/widgets/buttons.py HotkeyHoverHint._TEXT_STYLE).
const QString kHintTextStyle = QStringLiteral(R"(
        QLabel {
            color: @overlay-text;
            font-size: 11px;
            font-weight: 600;
            letter-spacing: 0.5px;
            font-family: "Segoe UI", "Helvetica Neue", sans-serif;
            background: transparent;
            border: none;
            padding: 0;
        }
    )");

} // namespace

HotkeyHoverHint::HotkeyHoverHint(QWidget* parent)
    : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setObjectName(QStringLiteral("hotkeyHoverHint"));
    // The pill is painted in paintEvent against a translucent window, so the rectangular window
    // corners stay invisible. QSS rounding on an opaque ToolTip window leaves square backing
    // blocks, and the native drop shadow re-outlines the rectangle.
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);

    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(12, 6, 12, 6);
    outer->setSpacing(0);

    textLabel_ = new QLabel;
    textLabel_->setAlignment(Qt::AlignCenter);
    ui::setLocalStyleSheet(textLabel_, kHintTextStyle);
    outer->addWidget(textLabel_);

    fadeIn_ = new QPropertyAnimation(this, "windowOpacity", this);
    fadeIn_->setDuration(140);
    fadeIn_->setStartValue(0.0);
    fadeIn_->setEndValue(1.0);
    fadeIn_->setEasingCurve(QEasingCurve::OutCubic);
}

void HotkeyHoverHint::setHotkey(const QString& hotkey) {
    textLabel_->setText(hotkey);
    adjustSize();
}

void HotkeyHoverHint::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    // Half-pixel inset keeps the 1 px border crisp instead of smeared across two device pixels.
    const QRectF rect = QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const double radius = rect.height() / 2.0;
    const ui::Theme& theme = ui::currentTheme();
    painter.setPen(QPen(theme.overlayBorder, 1.0));
    painter.setBrush(theme.overlayBg);
    painter.drawRoundedRect(rect, radius, radius);
}

void HotkeyHoverHint::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    fadeIn_->stop();
    setWindowOpacity(0.0);
    fadeIn_->start();
}

HotkeyHintFilter::HotkeyHintFilter(QPushButton* button, const QString& hotkey)
    : QObject(button), button_(button), hotkey_(hotkey) {
    hideTimer_ = new QTimer(this);
    hideTimer_->setSingleShot(true);
    hideTimer_->setInterval(kHideDelayMs);
    connect(hideTimer_, &QTimer::timeout, this, &HotkeyHintFilter::hideHint);
    button->installEventFilter(this);
}

void HotkeyHintFilter::setHotkey(const QString& hotkey) {
    hotkey_ = hotkey;
    if (hint_ != nullptr) {
        hint_->setHotkey(hotkey);
    }
}

bool HotkeyHintFilter::eventFilter(QObject* obj, QEvent* event) {
    if (obj == button_) {
        switch (event->type()) {
        case QEvent::Enter:
            showHint();
            break;
        case QEvent::Leave:
            hideTimer_->start();
            break;
        case QEvent::Hide:
            hideHint();
            break;
        default:
            break;
        }
    }
    return false;
}

void HotkeyHintFilter::showHint() {
    if (hotkey_.isEmpty() || !button_->isEnabled()) {
        return;
    }
    hideTimer_->stop();
    hint_ = showHotkeyHint(hint_, button_, button_->rect(), hotkey_);
}

void HotkeyHintFilter::hideHint() {
    hideHotkeyHint(hint_);
}

HotkeyHoverHint* showHotkeyHint(HotkeyHoverHint* hint, QWidget* anchor, const QRect& area,
                                const QString& hotkey) {
    if (hint == nullptr) {
        hint = new HotkeyHoverHint(anchor->window());
    }
    hint->setHotkey(hotkey);

    const QPoint topCenter = anchor->mapToGlobal(QPoint(area.center().x(), area.top()));
    const QSize size = hint->sizeHint();
    hint->move(topCenter.x() - size.width() / 2, topCenter.y() - size.height() - 8);
    hint->show();
    hint->raise();
    return hint;
}

void hideHotkeyHint(HotkeyHoverHint* hint) {
    if (hint != nullptr) {
        hint->hide();
    }
}

Button::Button(const QString& text, QWidget* parent) : QPushButton(text, parent) {
    setMinimumHeight(44);
    setFont(QFont(QStringLiteral("Segoe UI"), 12));
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);

    QSizePolicy policy = sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Minimum);
    setSizePolicy(policy);

    hotkeyHintFilter_ = new HotkeyHintFilter(this);
}

void Button::setText(const QString& text) {
    QPushButton::setText(text);
    refreshSize();
}

void Button::setHotkey(const QString& hotkey) {
    hotkeyHintFilter_->setHotkey(hotkey);
}

void Button::setBaseMinimumSize(int width, int height) {
    baseMinWidth_ = width;
    baseMinHeight_ = height;
    refreshSize();
}

void Button::setActive(bool active) {
    if (active_ == active) {
        return;
    }
    active_ = active;
    ui::setStyleProperty(this, ui::prop::inactive, !active);
    setCursor(active ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (!active) {
        hotkeyHintFilter_->hideHint();
    }
}

void Button::useToneSize() {
    baseMinHeight_ = 48;
    baseMinWidth_ = 140;
    setMinimumHeight(48);
    setMinimumWidth(140);
}

void Button::refreshSize() {
    ensurePolished();
    const QFontMetrics metrics = fontMetrics();
    const int textWidth = text().isEmpty() ? 0 : metrics.horizontalAdvance(text());
    constexpr int horizontalPadding = 40;
    // sizeHint includes stylesheet padding and borders; the font metrics alone can still
    // undershoot by a pixel or two.
    const int fitted = std::max(textWidth + horizontalPadding, sizeHint().width());
    setMinimumWidth(std::max(baseMinWidth_, fitted));
    setMinimumHeight(baseMinHeight_);
}

void Button::mousePressEvent(QMouseEvent* event) {
    if (!active_) {
        event->ignore();
        return;
    }
    QPushButton::mousePressEvent(event);
}

void Button::mouseReleaseEvent(QMouseEvent* event) {
    if (!active_) {
        event->ignore();
        return;
    }
    QPushButton::mouseReleaseEvent(event);
}

void Button::keyPressEvent(QKeyEvent* event) {
    if (!active_ && (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return ||
                     event->key() == Qt::Key_Enter)) {
        event->ignore();
        return;
    }
    QPushButton::keyPressEvent(event);
}

void fitCompactButton(Button* button, std::optional<int> width) {
    button->setBaseMinimumSize(width.value_or(0), 34);
    button->ensurePolished();
    const int height = std::max(34, button->sizeHint().height());
    button->setMinimumHeight(height);
    button->setMaximumHeight(height);
    if (!width) {
        return;
    }
    if (*width != 0) {
        const int fitted = std::max({*width, button->minimumWidth(), button->sizeHint().width()});
        button->setMinimumWidth(fitted);
        button->setMaximumWidth(fitted);
    } else {
        button->setMinimumWidth(0);
        button->setMaximumWidth(kUnlimitedHeight);
    }
}

Button* neutralButton(Button* button) {
    ui::setStyleProperty(button, ui::prop::tone, QStringLiteral("neutral"));
    button->setBaseMinimumSize(0, 34);
    return button;
}

Button* compactPrimaryButton(Button* button) {
    ui::setStyleProperty(button, ui::prop::tone, QStringLiteral("primary"));
    button->setBaseMinimumSize(0, 34);
    return button;
}

Button* compactDangerButton(Button* button) {
    ui::setStyleProperty(button, ui::prop::tone, QStringLiteral("danger"));
    button->setBaseMinimumSize(0, 34);
    return button;
}

QWidget* fitStartWidth(QWidget* widget) {
    widget->setMaximumWidth(kStartButtonWidth);
    QSizePolicy policy = widget->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Expanding);
    widget->setSizePolicy(policy);
    return widget;
}

PrimaryButton::PrimaryButton(const QString& text, QWidget* parent) : Button(text, parent) {
    setObjectName(QStringLiteral("primaryButton"));
    useToneSize();
}

DangerButton::DangerButton(const QString& text, QWidget* parent) : Button(text, parent) {
    setObjectName(QStringLiteral("dangerButton"));
    useToneSize();
}

SuccessButton::SuccessButton(const QString& text, QWidget* parent) : Button(text, parent) {
    setObjectName(QStringLiteral("successButton"));
    useToneSize();
}

WarningButton::WarningButton(const QString& text, QWidget* parent) : Button(text, parent) {
    setObjectName(QStringLiteral("warningButton"));
    useToneSize();
}

} // namespace fv
