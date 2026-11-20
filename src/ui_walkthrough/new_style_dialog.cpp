#include "ui_walkthrough/new_style_dialog.h"

#include <string>

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QVBoxLayout>

#include "pyport/errors.h"
#include "settings/store.h"
#include "ui_theme/local_style.h"
#include "ui_widgets/buttons.h"

namespace fv {

NewStyleDialog::NewStyleDialog(SettingsStore& settings, QWidget* parent)
    : QDialog(parent), settings_(settings) {
    setWindowTitle(QStringLiteral("New style"));
    setMinimumSize(480, 380);
    resize(540, 420);
    build();
}

void NewStyleDialog::build() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("New write-up style"));
    title->setObjectName(QStringLiteral("headerLabel"));
    title->setFont(QFont(QStringLiteral("Segoe UI"), 14, QFont::DemiBold));
    layout->addWidget(title);
    auto* info = new QLabel(QStringLiteral("Say what the AI write-up should be. The style shows "
                                           "under Start from here and in a storyboard's Redo as."));
    info->setObjectName(QStringLiteral("infoLabel"));
    info->setWordWrap(true);
    layout->addWidget(info);

    nameEdit_ = new QLineEdit();
    nameEdit_->setAccessibleName(QStringLiteral("Style name"));
    nameEdit_->setMaxLength(static_cast<int>(walkthrough::MAX_NAME));
    nameEdit_->setPlaceholderText(QStringLiteral("Name, such as Release notes"));
    nameEdit_->setToolTip(QStringLiteral("A name already saved is replaced."));
    layout->addWidget(nameEdit_);

    promptEdit_ = new QPlainTextEdit();
    promptEdit_->setAccessibleName(QStringLiteral("Style prompt"));
    promptEdit_->setPlaceholderText(QStringLiteral(
        "Say what you want, such as \"a short checklist for a new teammate, one screenshot per "
        "step\"."));
    promptEdit_->setTabChangesFocus(true);
    // The Walkthrough tab's field look; the theme has none for a bare QPlainTextEdit.
    ui::setLocalStyleSheet(promptEdit_,
                           QStringLiteral("QPlainTextEdit { color: @text; background-color: "
                                          "@slate-field; border: 1px solid @slate-border; "
                                          "border-radius: 10px; padding: 6px 8px; font-size: "
                                          "13px; } QPlainTextEdit:focus { border-color: @accent; "
                                          "}"));
    layout->addWidget(promptEdit_, 1);

    error_ = new QLabel(QString());
    ui::setLocalStyleSheet(error_,
                           QStringLiteral("QLabel { color: @danger-text-soft; font-size: 12px; }"));
    error_->setWordWrap(true);
    error_->hide();
    layout->addWidget(error_);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    auto* cancel = new Button(QStringLiteral("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancel);
    saveButton_ = new PrimaryButton(QStringLiteral("Save style"));
    connect(saveButton_, &QPushButton::clicked, this, [this] { save(); });
    buttons->addWidget(saveButton_);
    layout->addLayout(buttons);
    nameEdit_->setFocus();
}

void NewStyleDialog::save() {
    const std::string name = nameEdit_->text().toStdString();
    const std::string words = promptEdit_->toPlainText().toStdString();
    std::optional<walkthrough::WriteUpStyle> made;
    try {
        settings_.mutate(
            [&](Json& settings) { made = walkthrough::save_style(settings, name, words); });
    } catch (const py::ValueError& failure) {
        error_->setText(QString::fromUtf8(failure.what()));
        error_->show();
        return;
    }
    saved_ = std::move(made);
    accept();
}

} // namespace fv
