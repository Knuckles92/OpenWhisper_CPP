#include "ui_personalize/command_shortcut_field.h"

#include <algorithm>

#include <QBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "pyport/number.h"
#include "settings/keys.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_personalize/personalize_host.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

const QString& captureTip() {
    static const QString tip = QStringLiteral("Click, then press a shortcut. Escape cancels.");
    return tip;
}

int scaledInputWidth() {
    return static_cast<int>(
        py::round_half_even(CommandShortcutField::kInputMinWidth * ui::currentUiFontScale()));
}

// A label-fitted Button holds a floor above its size hint; Qt draws the box over a button
// squeezed below it.
int buttonWidth(const QPushButton* button) {
    return std::max(button->sizeHint().width(), button->minimumWidth());
}

QString commandHotkey(const Json& hotkeys) {
    if (!hotkeys.is_object()) {
        return {};
    }
    const auto found = hotkeys.find(CommandShortcutField::kAction);
    return found != hotkeys.end() && found->is_string() ? qt::toQString(found->get<std::string>())
                                                        : QString();
}

} // namespace

CommandShortcutField::CommandShortcutField(PersonalizeHost& host, bool changeButton,
                                           bool clearButton, bool inlineErrors, QWidget* parent)
    : QWidget(parent), host_(host), inlineErrors_(inlineErrors) {
    ui::setStyleProperty(this, ui::prop::tileId, QStringLiteral("commandShortcutField"));
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(6);
    row_ = new QBoxLayout(QBoxLayout::LeftToRight);
    row_->setContentsMargins(0, 0, 0, 0);
    row_->setSpacing(8);
    input_ = new ProfileHotkeyInput;
    input_->setAccessibleName(QStringLiteral("Command Mode shortcut"));
    input_->setToolTip(captureTip());
    input_->setMinimumWidth(kInputMinWidth);
    input_->setMaximumWidth(kInputMaxWidth);
    connect(input_, &ProfileHotkeyInput::captureChanged, this,
            [this](bool capturing) { host_.setHotkeyCaptureSuspended(capturing); });
    connect(input_, &ProfileHotkeyInput::captured, this, &CommandShortcutField::save);
    row_->addWidget(input_, 1);
    if (changeButton) {
        // Plain, like the Recording shortcut's Change on Home.
        change_ = new QPushButton(QStringLiteral("Change"));
        change_->setObjectName(QStringLiteral("settingsChangeShortcut"));
        connect(change_, &QPushButton::clicked, this, &CommandShortcutField::beginCapture);
        row_->addWidget(change_);
    }
    if (clearButton) {
        clear_ = neutralButton(new Button(QStringLiteral("Clear")));
        clear_->setBaseMinimumSize(64, 34);
        clear_->setToolTip(QStringLiteral("Remove the Command Mode shortcut"));
        connect(clear_, &QPushButton::clicked, this, [this] { save(QString()); });
        row_->addWidget(clear_);
    }
    row_->addStretch();
    column->addLayout(row_);
    message_ = new WrappedLabel;
    message_->setObjectName(QStringLiteral("infoLabel"));
    ui::setStyleProperty(message_, ui::prop::tileId, QStringLiteral("commandShortcutError"));
    message_->setTextFormat(Qt::PlainText);
    message_->hide();
    column->addWidget(message_);
    sync();
}

QString CommandShortcutField::hotkey() const {
    Json hotkeys = host_.currentHotkeys();
    if (!hotkeys.is_object() || hotkeys.empty()) {
        const Json settings = host_.settings();
        hotkeys = settings.is_object() ? keys::hotkeys.read(settings) : Json();
    }
    return commandHotkey(hotkeys);
}

void CommandShortcutField::sync() {
    if (!input_->isCapturing()) {
        input_->setHotkey(hotkey());
    }
    if (clear_ != nullptr) {
        clear_->setEnabled(!hotkey().isEmpty());
    }
}

void CommandShortcutField::beginCapture() {
    input_->setFocus();
    input_->beginCapture();
}

void CommandShortcutField::cancelCapture() {
    input_->cancelCapture();
}

void CommandShortcutField::showError(const QString& text) {
    message_->setText(text);
    message_->setVisible(!text.isEmpty() && inlineErrors_);
}

void CommandShortcutField::save(const QString& hotkey) {
    const QString error = host_.setStandardHotkey(QString::fromUtf8(kAction), hotkey);
    showError(error);
    sync();
}

void CommandShortcutField::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    sync();
}

std::vector<QPushButton*> CommandShortcutField::buttons() const {
    std::vector<QPushButton*> found;
    if (change_ != nullptr) {
        found.push_back(change_);
    }
    if (clear_ != nullptr) {
        found.push_back(clear_);
    }
    return found;
}

int CommandShortcutField::sideBySideWidth() const {
    int width = scaledInputWidth();
    for (const QPushButton* button : buttons()) {
        width += row_->spacing() + buttonWidth(button);
    }
    return width;
}

// Both hints are the same whichever way resizeEvent last arranged the field, so the width a
// layout gives it never depends on that choice. When they followed the arrangement, a row could
// give the field less room side by side than stacked, and it flipped between the two forever.
QSize CommandShortcutField::sizeHint() const {
    return {sideBySideWidth(), QWidget::sizeHint().height()};
}

QSize CommandShortcutField::minimumSizeHint() const {
    int widest = 0;
    for (const QPushButton* button : buttons()) {
        widest = std::max(widest, buttonWidth(button));
    }
    return {widest, QWidget::minimumSizeHint().height()};
}

void CommandShortcutField::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const int box = scaledInputWidth();
    const bool stacked = width() < sideBySideWidth();
    input_->setMinimumWidth(stacked ? std::min(box, width()) : box);
    const QBoxLayout::Direction direction =
        stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    if (row_->direction() != direction) {
        row_->setDirection(direction);
        for (QPushButton* button : buttons()) {
            row_->setAlignment(button, stacked ? Qt::AlignLeft : Qt::Alignment());
        }
    }
}

} // namespace fv
