#include "ui_personalize/cleanup_rule_dialog.h"

#include <utility>

#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QTextEdit>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "pyport/text.h"
#include "ui_widgets/buttons.h"

namespace fv {
namespace {

// str.strip() on a QString.
QString stripped(const QString& text) {
    return qt::toQString(py::strip(qt::toUtf8(text)));
}

QString casefolded(const QString& text) {
    return qt::toQString(py::casefold(qt::toUtf8(text)));
}

} // namespace

CleanupRuleDialog::CleanupRuleDialog(QString rule, std::optional<QString> original, QString notice,
                                     bool dictated, QWidget* parent)
    : QDialog(parent), original_(stripped(original.value_or(QString()))), polished_(stripped(rule)),
      dictated_(dictated) {
    setObjectName(QStringLiteral("cleanupRuleDialog"));
    offerChoice_ = original.has_value() && notice.isEmpty() && !original_.isEmpty() &&
                   !polished_.isEmpty() && casefolded(polished_) != casefolded(original_);
    setWindowTitle(original.has_value() ? QStringLiteral("Confirm Learned Rule")
                                        : QStringLiteral("Edit Learned Rule"));
    setMinimumSize(620, offerChoice_ ? 330 : 300);
    resize(660, offerChoice_ ? 410 : 360);
    setupUi(rule, original, notice);
}

void CleanupRuleDialog::setupUi(const QString& rule, const std::optional<QString>& original,
                                const QString& notice) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    auto* header = new QHBoxLayout;
    header->setSpacing(14);
    auto* mark = new QLabel(original.has_value() ? QStringLiteral("AI") : QStringLiteral("Aa"));
    mark->setObjectName(QStringLiteral("cleanupRuleDialogMark"));
    mark->setAlignment(Qt::AlignCenter);
    mark->setFixedSize(44, 44);
    header->addWidget(mark);

    auto* heading = new QVBoxLayout;
    heading->setSpacing(2);
    auto* title = new QLabel(original.has_value() ? QStringLiteral("Review your new rule")
                                                  : QStringLiteral("Edit learned rule"));
    title->setObjectName(QStringLiteral("cleanupRuleDialogTitle"));
    title->setFont(QFont(QStringLiteral("Segoe UI"), 15, QFont::DemiBold));
    heading->addWidget(title);

    auto* subtitle = new QLabel(
        QStringLiteral("This instruction becomes part of your personal cleanup profile."));
    subtitle->setObjectName(QStringLiteral("cleanupRuleDialogSubtitle"));
    subtitle->setWordWrap(true);
    heading->addWidget(subtitle);
    header->addLayout(heading, 1);
    layout->addLayout(header);

    if (original.has_value() && !stripped(*original).isEmpty()) {
        const QString source = dictated_ ? QStringLiteral("You said") : QStringLiteral("You typed");
        auto* said = new QLabel(QStringLiteral("%1  ·  “%2”").arg(source, stripped(*original)));
        said->setObjectName(QStringLiteral("cleanupRuleOriginal"));
        said->setWordWrap(true);
        layout->addWidget(said);
    }

    if (!notice.isEmpty()) {
        auto* warn = new QLabel(notice);
        warn->setObjectName(QStringLiteral("cleanupRuleNotice"));
        warn->setWordWrap(true);
        layout->addWidget(warn);
    }

    QLabel* info = nullptr;
    if (offerChoice_) {
        const QString keep = dictated_ ? QStringLiteral("your words as transcribed")
                                       : QStringLiteral("exactly what you typed");
        info = new QLabel(
            QStringLiteral("AI polished your instruction into a clearer rule for the cleanup "
                           "prompt. We recommend the polished version, or you can keep %1. "
                           "Edit either choice below before saving.")
                .arg(keep));
    } else {
        info = new QLabel(
            QStringLiteral("This rule is added to the cleanup prompt on every transcript. "
                           "Edit it if needed, then save."));
    }
    info->setObjectName(QStringLiteral("cleanupRuleDialogInfo"));
    info->setWordWrap(true);
    layout->addWidget(info);

    auto* editor = new QFrame;
    editor->setObjectName(QStringLiteral("cleanupRuleEditorCard"));
    auto* editorLayout = new QVBoxLayout(editor);
    editorLayout->setContentsMargins(14, 12, 14, 14);
    editorLayout->setSpacing(8);

    auto* editorLabel = new QLabel(QStringLiteral("RULE INSTRUCTION"));
    editorLabel->setObjectName(QStringLiteral("cleanupRuleEditorLabel"));
    editorLayout->addWidget(editorLabel);

    ruleEdit_ = new QTextEdit;
    ruleEdit_->setObjectName(QStringLiteral("cleanupRuleEditor"));
    ruleEdit_->setAcceptRichText(false);
    ruleEdit_->setFont(QFont(QStringLiteral("Segoe UI"), 12));
    ruleEdit_->setPlainText(rule);
    ruleEdit_->setPlaceholderText(QStringLiteral("Enter the rule…"));
    ruleEdit_->setMinimumHeight(80);
    editorLayout->addWidget(ruleEdit_, 1);
    layout->addWidget(editor, 1);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(10);
    buttons->addStretch();

    auto* cancel = new Button(QStringLiteral("Cancel"));
    cancel->setBaseMinimumSize(92, 42);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancel);

    if (offerChoice_) {
        auto* asTyped = new Button(dictated_ ? QStringLiteral("Use What I Said")
                                             : QStringLiteral("Use Exactly as Typed"));
        asTyped->setBaseMinimumSize(156, 42);
        asTyped->setToolTip(QStringLiteral("Save your original wording without AI changes"));
        connect(asTyped, &QPushButton::clicked, this, [this] { acceptAsTyped(); });
        buttons->addWidget(asTyped);

        auto* polished = new PrimaryButton(QStringLiteral("Use Polished (Recommended)"));
        polished->setBaseMinimumSize(190, 42);
        polished->setToolTip(QStringLiteral("Save the AI-polished rule (or your edits to it)"));
        connect(polished, &QPushButton::clicked, this, [this] { acceptPolished(); });
        buttons->addWidget(polished);
    } else {
        auto* save = new PrimaryButton(QStringLiteral("Save Rule"));
        save->setBaseMinimumSize(112, 42);
        connect(save, &QPushButton::clicked, this, &QDialog::accept);
        buttons->addWidget(save);
    }

    layout->addLayout(buttons);
}

void CleanupRuleDialog::acceptAsTyped() {
    ruleEdit_->setPlainText(original_);
    accept();
}

void CleanupRuleDialog::acceptPolished() {
    // "Use Polished" falls back to the polished text when the editor was emptied.
    if (stripped(ruleEdit_->toPlainText()).isEmpty()) {
        ruleEdit_->setPlainText(polished_);
    }
    accept();
}

QString CleanupRuleDialog::ruleText() const {
    return stripped(ruleEdit_->toPlainText());
}

} // namespace fv
