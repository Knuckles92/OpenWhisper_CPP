#include "ui_personalize/cleanup_prompt_dialog.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QTextEdit>
#include <QVBoxLayout>

#include "ui_widgets/buttons.h"

namespace fv {

CleanupPromptDialog::CleanupPromptDialog(const QString& prompt, QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Edit Cleanup Prompt"));
    setMinimumSize(520, 420);
    resize(560, 480);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("Cleanup Prompt"));
    title->setObjectName(QStringLiteral("headerLabel"));
    title->setFont(QFont(QStringLiteral("Segoe UI"), 14, QFont::DemiBold));
    layout->addWidget(title);

    auto* info =
        new QLabel(QStringLiteral("Instructions sent to the AI when cleaning up a transcript. "
                                  "Describe how you want the text rewritten."));
    info->setObjectName(QStringLiteral("infoLabel"));
    info->setWordWrap(true);
    layout->addWidget(info);

    promptEdit_ = new QTextEdit;
    promptEdit_->setAcceptRichText(false);
    promptEdit_->setFont(QFont(QStringLiteral("Segoe UI"), 12));
    promptEdit_->setPlainText(prompt);
    promptEdit_->setPlaceholderText(QStringLiteral("Enter cleanup instructions…"));
    layout->addWidget(promptEdit_, 1);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();

    auto* cancel = new Button(QStringLiteral("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancel);

    auto* save = new PrimaryButton(QStringLiteral("Use Prompt"));
    connect(save, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(save);

    layout->addLayout(buttons);
}

QString CleanupPromptDialog::promptText() const {
    return promptEdit_->toPlainText();
}

} // namespace fv
