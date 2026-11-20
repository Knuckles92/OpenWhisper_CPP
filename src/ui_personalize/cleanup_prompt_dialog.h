#pragma once

#include <QDialog>
#include <QString>

class QTextEdit;

namespace fv {

// Edits the instructions sent to the AI when cleaning up a transcript
// (ui_qt/dialogs/cleanup_prompt_dialog.py, "Edit Cleanup Prompt").
// Affinity: GUI thread.
class CleanupPromptDialog : public QDialog {
    Q_OBJECT
public:
    explicit CleanupPromptDialog(const QString& prompt, QWidget* parent = nullptr);

    // The text as typed, not trimmed (prompt_text).
    QString promptText() const;
    QTextEdit* promptEdit() const { return promptEdit_; }

private:
    QTextEdit* promptEdit_ = nullptr;
};

} // namespace fv
