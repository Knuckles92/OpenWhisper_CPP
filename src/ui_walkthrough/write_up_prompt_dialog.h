#pragma once

#include <vector>

#include <QDialog>
#include <QString>

#include "walkthrough/styles.h"

class QLabel;
class QLineEdit;
class QPlainTextEdit;

// Write a walkthrough up again with a prompt typed for it, maybe kept as a style
// (ui_qt/dialogs/write_up_prompt_dialog.py). The prompt is for one write-up: Settings' prompt for
// the next walkthroughs stays. Affinity: GUI thread.
namespace fv {

class Button;
class NoWheelComboBox;
class SettingsStore;

class WriteUpPromptDialog : public QDialog {
    Q_OBJECT

public:
    // The style menu's first item while the prompt is the user's own words.
    static constexpr const char* kOwnWords = "Your own words";

    // `prompt` is where the editor starts; `hasWriteUp` says whether there is a write-up to redo
    // ("Redo write-up") or not yet ("Write up with AI"). Naming the prompt saves it as a style
    // in `settings`, which must outlive the dialog.
    WriteUpPromptDialog(SettingsStore& settings, const QString& prompt, bool hasWriteUp = true,
                        QWidget* parent = nullptr);

    // The prompt, trimmed (prompt_text).
    QString text() const;

    // Redo write-up / Write it up: refuses an empty prompt, saves a named one as a style first
    // (its error shows and the dialog stays), then accepts.
    void go();

    NoWheelComboBox* styleCombo() const { return styleCombo_; }
    QPlainTextEdit* promptEdit() const { return promptEdit_; }
    QLineEdit* nameEdit() const { return nameEdit_; }
    QLabel* errorLabel() const { return error_; }
    Button* goButton() const { return goButton_; }

private:
    void build(const QString& prompt, bool hasWriteUp);
    void styleChosen(int index);
    // Points the menu at the style the prompt is, or at the user's own words.
    void showStyle();
    void fail(const QString& message);

    SettingsStore& settings_;
    std::vector<walkthrough::WriteUpStyle> styles_;
    NoWheelComboBox* styleCombo_ = nullptr;
    QPlainTextEdit* promptEdit_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QLabel* error_ = nullptr;
    Button* goButton_ = nullptr;
};

} // namespace fv
