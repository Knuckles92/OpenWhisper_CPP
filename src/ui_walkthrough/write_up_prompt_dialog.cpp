#include "ui_walkthrough/write_up_prompt_dialog.h"

#include <optional>
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
#include "ui_widgets/no_wheel.h"

namespace fv {

namespace {

QString qs(const std::string& value) {
    return QString::fromStdString(value);
}

} // namespace

WriteUpPromptDialog::WriteUpPromptDialog(SettingsStore& settings, const QString& prompt,
                                         bool hasWriteUp, QWidget* parent)
    : QDialog(parent), settings_(settings),
      styles_(walkthrough::load_styles(*settings.snapshot())) {
    setWindowTitle(hasWriteUp ? QStringLiteral("Redo write-up")
                              : QStringLiteral("Write up with AI"));
    setMinimumSize(520, 440);
    resize(580, 500);
    build(prompt, hasWriteUp);
}

void WriteUpPromptDialog::build(const QString& prompt, bool hasWriteUp) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("What should the write-up be?"));
    title->setObjectName(QStringLiteral("headerLabel"));
    title->setFont(QFont(QStringLiteral("Segoe UI"), 14, QFont::DemiBold));
    layout->addWidget(title);
    auto* info = new QLabel(
        QStringLiteral("The agent writes it up from your words as spoken, with this prompt") +
        (hasWriteUp ? QStringLiteral(" instead. The current write-up is kept as an earlier "
                                     "version. ")
                    : QStringLiteral(". ")) +
        QStringLiteral("Your prompt for new walkthroughs doesn't change."));
    info->setObjectName(QStringLiteral("infoLabel"));
    info->setWordWrap(true);
    layout->addWidget(info);

    auto* head = new QHBoxLayout();
    head->addWidget(new QLabel(QStringLiteral("Start from")));
    styleCombo_ = new NoWheelComboBox();
    styleCombo_->setAccessibleName(QStringLiteral("Write-up style"));
    for (const auto& style : styles_) {
        styleCombo_->addItem(qs(style.name), qs(style.instructions));
    }
    connect(styleCombo_, &QComboBox::activated, this, [this](int index) { styleChosen(index); });
    head->addWidget(styleCombo_, 1);
    layout->addLayout(head);

    promptEdit_ = new QPlainTextEdit();
    promptEdit_->setAccessibleName(QStringLiteral("Write-up prompt"));
    promptEdit_->setPlaceholderText(
        QStringLiteral("Say what you want, such as \"a short checklist for a new teammate, one "
                       "screenshot per step\"."));
    promptEdit_->setTabChangesFocus(true);
    promptEdit_->setPlainText(prompt);
    connect(promptEdit_, &QPlainTextEdit::textChanged, this, [this] { showStyle(); });
    layout->addWidget(promptEdit_, 1);

    nameEdit_ = new QLineEdit();
    nameEdit_->setAccessibleName(QStringLiteral("Style name"));
    nameEdit_->setMaxLength(static_cast<int>(walkthrough::MAX_NAME));
    nameEdit_->setPlaceholderText(QStringLiteral("Name it to keep it as a style (optional)"));
    nameEdit_->setToolTip(QStringLiteral("Saved styles show in the Redo as menu and the "
                                         "Walkthrough tab. A name already saved is replaced."));
    layout->addWidget(nameEdit_);

    error_ = new QLabel(QString());
    ui::setLocalStyleSheet(error_, QStringLiteral("QLabel { color: @danger-text-soft; font-size: "
                                                  "12px; }"));
    error_->setWordWrap(true);
    error_->hide();
    layout->addWidget(error_);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    auto* cancel = new Button(QStringLiteral("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancel);
    goButton_ = new PrimaryButton(hasWriteUp ? QStringLiteral("Redo write-up")
                                             : QStringLiteral("Write it up"));
    connect(goButton_, &QPushButton::clicked, this, [this] { go(); });
    buttons->addWidget(goButton_);
    layout->addLayout(buttons);
    showStyle();
    promptEdit_->setFocus();
    promptEdit_->selectAll();
}

QString WriteUpPromptDialog::text() const {
    return promptEdit_->toPlainText().trimmed();
}

void WriteUpPromptDialog::styleChosen(int index) {
    const QVariant words = styleCombo_->itemData(index);
    if (words.isValid() && words.toString() != text()) {
        promptEdit_->setPlainText(words.toString());
    }
}

void WriteUpPromptDialog::showStyle() {
    const QString prompt = text();
    std::optional<walkthrough::WriteUpStyle> style;
    if (!prompt.isEmpty()) {
        style = walkthrough::style_for(prompt.toStdString(), styles_);
    }
    const QString own = QString::fromLatin1(kOwnWords);
    const int at = styleCombo_->findText(own);
    if (!style && at < 0) {
        styleCombo_->insertItem(0, own);
    } else if (style && at >= 0) {
        styleCombo_->removeItem(at);
    }
    styleCombo_->setCurrentIndex(style ? styleCombo_->findText(qs(style->name))
                                       : styleCombo_->findText(own));
}

void WriteUpPromptDialog::go() {
    const QString prompt = text();
    if (prompt.isEmpty()) {
        fail(QStringLiteral("Type what you want the write-up to be."));
        return;
    }
    const QString name = nameEdit_->text().trimmed();
    if (!name.isEmpty()) {
        try {
            settings_.mutate([&](Json& settings) {
                walkthrough::save_style(settings, name.toStdString(), prompt.toStdString());
            });
        } catch (const py::ValueError& failure) {
            // The reason to fix, said where the user looks.
            fail(QString::fromUtf8(failure.what()));
            return;
        }
    }
    accept();
}

void WriteUpPromptDialog::fail(const QString& message) {
    error_->setText(message);
    error_->show();
}

} // namespace fv
