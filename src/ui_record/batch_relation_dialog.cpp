#include "ui_record/batch_relation_dialog.h"

#include <QCheckBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QTextEdit>
#include <QVBoxLayout>

#include "ui_record/detail/code_points.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

BatchRelationDialog::BatchRelationDialog(const QStringList& files, const QString& instructions,
                                         bool combine, QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("batchRelationDialog"));
    setWindowTitle(QStringLiteral("Describe These Files"));
    setModal(true);
    setMinimumSize(560, 460);
    resize(620, 520);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    auto* title = new QLabel(QStringLiteral("Describe these files"));
    title->setObjectName(QStringLiteral("batchRelationTitle"));
    title->setFont(QFont(QStringLiteral("Segoe UI"), 15, QFont::DemiBold));
    layout->addWidget(title);

    auto* info = new WrappedLabel(QStringLiteral(
        "Tell the cleanup model what these recordings are and how it should treat them: who is "
        "speaking, what belongs together, what to drop. The transcripts themselves are never "
        "treated as instructions."));
    info->setObjectName(QStringLiteral("batchRelationInfo"));
    layout->addWidget(info);

    auto* filesCard = new QFrame;
    filesCard->setObjectName(QStringLiteral("batchRelationFilesCard"));
    auto* filesLayout = new QVBoxLayout(filesCard);
    filesLayout->setContentsMargins(14, 12, 14, 12);
    filesLayout->setSpacing(4);
    auto* filesLabel = new QLabel(QStringLiteral(u"FILES IN THIS BATCH  ·  %1").arg(files.size()));
    filesLabel->setObjectName(QStringLiteral("batchRelationFilesLabel"));
    filesLayout->addWidget(filesLabel);
    for (qsizetype index = 0; index < std::min<qsizetype>(files.size(), kBatchDialogListedFiles);
         ++index) {
        auto* row = new ElidingLabel(files[index]);
        row->setObjectName(QStringLiteral("batchRelationFileName"));
        filesLayout->addWidget(row);
    }
    const qsizetype hidden = files.size() - kBatchDialogListedFiles;
    if (hidden > 0) {
        auto* more = new QLabel(QStringLiteral("+%1 more").arg(hidden));
        more->setObjectName(QStringLiteral("batchRelationFileName"));
        filesLayout->addWidget(more);
    }
    layout->addWidget(filesCard);

    auto* editor = new QFrame;
    editor->setObjectName(QStringLiteral("batchRelationEditorCard"));
    auto* editorLayout = new QVBoxLayout(editor);
    editorLayout->setContentsMargins(14, 12, 14, 14);
    editorLayout->setSpacing(8);

    auto* editorLabel = new QLabel(QStringLiteral("HOW THEY RELATE"));
    editorLabel->setObjectName(QStringLiteral("batchRelationEditorLabel"));
    editorLayout->addWidget(editorLabel);

    instructionsEdit_ = new QTextEdit;
    instructionsEdit_->setObjectName(QStringLiteral("batchRelationEditor"));
    instructionsEdit_->setAcceptRichText(false);
    instructionsEdit_->setFont(QFont(QStringLiteral("Segoe UI"), 12));
    instructionsEdit_->setPlainText(instructions);
    instructionsEdit_->setPlaceholderText(QStringLiteral(
        "e.g. Three interviews with the same client, recorded on different days. Keep each one "
        "separate and drop the small talk at the start."));
    instructionsEdit_->setMinimumHeight(110);
    editorLayout->addWidget(instructionsEdit_, 1);
    layout->addWidget(editor, 1);

    combineCheck_ = new QCheckBox(QStringLiteral("Combine into one transcript"));
    combineCheck_->setObjectName(QStringLiteral("batchRelationCombine"));
    combineCheck_->setChecked(combine);
    combineCheck_->setToolTip(QStringLiteral(
        "On: the files are joined in order and cleaned as one transcript with one History entry. "
        "Off: each file is cleaned and saved on its own."));
    layout->addWidget(combineCheck_);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(10);
    buttons->addStretch();

    cancelButton_ = new Button(QStringLiteral("Cancel"));
    cancelButton_->setBaseMinimumSize(92, 42);
    cancelButton_->setAutoDefault(false);
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancelButton_);

    useButton_ = new PrimaryButton(QStringLiteral("Use Description"));
    useButton_->setBaseMinimumSize(140, 42);
    useButton_->setDefault(true);
    connect(useButton_, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(useButton_);
    layout->addLayout(buttons);

    instructionsEdit_->setFocus(Qt::OtherFocusReason);
}

QString BatchRelationDialog::instructionsText() const {
    const QString text = instructionsEdit_->toPlainText().trimmed();
    return text.left(detail::prefixLength(text, MAX_TRANSCRIPT_BATCH_INSTRUCTION_CHARS, nullptr));
}

bool BatchRelationDialog::combineChecked() const {
    return combineCheck_->isChecked();
}

} // namespace fv
