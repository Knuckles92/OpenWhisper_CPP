#include "ui_widgets/no_wheel.h"

#include <algorithm>

#include <QFrame>
#include <QIcon>
#include <QLineEdit>
#include <QPalette>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace fv {
namespace {

QIcon assetIcon(const QString& file) {
    return QIcon(QStringLiteral(":/ui_theme/assets/") + file);
}

} // namespace

NoWheelComboBox::NoWheelComboBox(QWidget* parent) : QComboBox(parent) {
    setFocusPolicy(Qt::StrongFocus);
}

void NoWheelComboBox::wheelEvent(QWheelEvent* event) {
    if (hasFocus()) {
        QComboBox::wheelEvent(event);
    } else {
        event->ignore();
    }
}

ElidingComboBox::ElidingComboBox(QWidget* parent) : NoWheelComboBox(parent) {
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

QSize ElidingComboBox::minimumSizeHint() const {
    const QSize hint = QComboBox::minimumSizeHint();
    return QSize(std::min(hint.width(), kMinimumWidth), hint.height());
}

void ElidingComboBox::paintEvent(QPaintEvent*) {
    QStylePainter painter(this);
    painter.setPen(palette().color(QPalette::Text));
    QStyleOptionComboBox option;
    initStyleOption(&option);
    painter.drawComplexControl(QStyle::CC_ComboBox, option);
    const QRect field =
        style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this);
    option.currentText =
        fontMetrics().elidedText(option.currentText, Qt::ElideRight, field.width());
    painter.drawControl(QStyle::CE_ComboBoxLabel, option);
}

void ElidingComboBox::showPopup() {
    QAbstractItemView* popup = view();
    QScrollBar* scrollbar = popup->verticalScrollBar();
    int extra = 2 * popup->frameWidth();
    if (count() > maxVisibleItems()) {
        extra += scrollbar->sizeHint().width();
    }
    popup->setMinimumWidth(popup->sizeHintForColumn(0) + extra);
    QComboBox::showPopup();
}

NoWheelSpinBox::NoWheelSpinBox(QWidget* parent) : QSpinBox(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setButtonSymbols(QAbstractSpinBox::NoButtons);

    stepper_ = new QWidget(this);
    stepper_->setObjectName(QStringLiteral("spinStepper"));
    stepper_->setAttribute(Qt::WA_StyledBackground, true);
    auto* rail = new QVBoxLayout(stepper_);
    rail->setContentsMargins(0, 0, 0, 0);
    rail->setSpacing(0);

    upButton_ = makeStepButton("spinStepUp", QStringLiteral("spin-chevron-up.svg"),
                               QStringLiteral("Increase"));
    downButton_ = makeStepButton("spinStepDown", QStringLiteral("spin-chevron-down.svg"),
                                 QStringLiteral("Decrease"));
    connect(upButton_, &QToolButton::clicked, this, [this] { step(1); });
    connect(downButton_, &QToolButton::clicked, this, [this] { step(-1); });
    auto* divider = new QFrame(stepper_);
    divider->setObjectName(QStringLiteral("spinStepperDivider"));
    divider->setAttribute(Qt::WA_StyledBackground, true);
    divider->setFixedHeight(1);
    divider->setFrameShape(QFrame::NoFrame);
    rail->addWidget(upButton_);
    rail->addWidget(divider);
    rail->addWidget(downButton_);

    connect(this, &QSpinBox::valueChanged, this, [this](int) { syncStepButtons(); });
    syncStepButtons();
    layoutStepper();
}

QToolButton* NoWheelSpinBox::makeStepButton(const char* objectName, const QString& icon,
                                            const QString& tooltip) {
    auto* button = new QToolButton(stepper_);
    button->setObjectName(QString::fromLatin1(objectName));
    button->setIcon(assetIcon(icon));
    button->setIconSize(QSize(kIconSize, kIconSize));
    button->setToolTip(tooltip);
    button->setAutoRepeat(true);
    button->setAutoRepeatDelay(400);
    button->setAutoRepeatInterval(80);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setMinimumSize(0, 0);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    return button;
}

void NoWheelSpinBox::step(int steps) {
    if (!hasFocus()) {
        setFocus(Qt::MouseFocusReason);
    }
    stepBy(steps);
}

void NoWheelSpinBox::syncStepButtons() {
    const StepEnabled flags = stepEnabled();
    upButton_->setEnabled(flags.testFlag(StepUpEnabled));
    downButton_->setEnabled(flags.testFlag(StepDownEnabled));
}

void NoWheelSpinBox::layoutStepper() {
    constexpr int frame = 1;
    constexpr int width = kStepperWidth;
    stepper_->setGeometry(std::max(0, this->width() - width - frame), frame, width,
                          std::max(0, height() - 2 * frame));
    stepper_->raise();
    if (QLineEdit* edit = lineEdit()) {
        edit->setTextMargins(0, 0, width + kStepperGap, 0);
    }
}

QSize NoWheelSpinBox::sizeHint() const {
    const QSize hint = QSpinBox::sizeHint();
    return QSize(hint.width() + kStepperWidth + kStepperGap, hint.height());
}

QSize NoWheelSpinBox::minimumSizeHint() const {
    const QSize hint = QSpinBox::minimumSizeHint();
    return QSize(hint.width() + kStepperWidth + kStepperGap, hint.height());
}

void NoWheelSpinBox::setSpecialValueText(const QString& text) {
    QSpinBox::setSpecialValueText(text);
    updateGeometry();
}

void NoWheelSpinBox::setMinimum(int min) {
    QSpinBox::setMinimum(min);
    syncStepButtons();
}

void NoWheelSpinBox::setMaximum(int max) {
    QSpinBox::setMaximum(max);
    syncStepButtons();
}

void NoWheelSpinBox::setRange(int min, int max) {
    QSpinBox::setRange(min, max);
    syncStepButtons();
}

void NoWheelSpinBox::setWrapping(bool wrapping) {
    QSpinBox::setWrapping(wrapping);
    syncStepButtons();
}

void NoWheelSpinBox::resizeEvent(QResizeEvent* event) {
    QSpinBox::resizeEvent(event);
    layoutStepper();
}

void NoWheelSpinBox::showEvent(QShowEvent* event) {
    QSpinBox::showEvent(event);
    layoutStepper();
}

void NoWheelSpinBox::wheelEvent(QWheelEvent* event) {
    if (hasFocus()) {
        QSpinBox::wheelEvent(event);
    } else {
        event->ignore();
    }
}

NoWheelDateEdit::NoWheelDateEdit(QWidget* parent) : QDateEdit(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setCalendarPopup(true);
}

void NoWheelDateEdit::wheelEvent(QWheelEvent* event) {
    if (hasFocus()) {
        QDateEdit::wheelEvent(event);
    } else {
        event->ignore();
    }
}

} // namespace fv
