#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QCheckBox;
class QTextEdit;

namespace fv {

class Button;
class PrimaryButton;

// config.MAX_TRANSCRIPT_BATCH_INSTRUCTION_CHARS: the longest description the dialog returns, in
// code points.
inline constexpr int MAX_TRANSCRIPT_BATCH_INSTRUCTION_CHARS = 2000;
// File names listed before the rest collapse into "+N more".
inline constexpr int kBatchDialogListedFiles = 6;

// Describes how the files of a multi-file upload relate, for the cleanup model: the Custom
// preset's description and output shape. Modal. Affinity: GUI thread.
class BatchRelationDialog : public QDialog {
    Q_OBJECT
public:
    // files: the queued files, shown for reference while writing. instructions: the remembered
    // description, prefilled. combine: whether the remembered choice was one combined transcript.
    explicit BatchRelationDialog(const QStringList& files, const QString& instructions = {},
                                 bool combine = false, QWidget* parent = nullptr);

    // The description stripped and cut to MAX_TRANSCRIPT_BATCH_INSTRUCTION_CHARS code points.
    QString instructionsText() const;
    bool combineChecked() const;

    QTextEdit* instructionsEdit() const { return instructionsEdit_; }
    QCheckBox* combineCheck() const { return combineCheck_; }
    Button* cancelButton() const { return cancelButton_; }
    PrimaryButton* useButton() const { return useButton_; }

private:
    QTextEdit* instructionsEdit_;
    QCheckBox* combineCheck_;
    Button* cancelButton_;
    PrimaryButton* useButton_;
};

} // namespace fv
