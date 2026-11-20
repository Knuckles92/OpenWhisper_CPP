#pragma once

#include <optional>

#include <QDialog>

#include "walkthrough/styles.h"

class QLabel;
class QLineEdit;
class QPlainTextEdit;

namespace fv {

class PrimaryButton;
class SettingsStore;

// Make a write-up style from scratch: a name and the words it fills in
// (ui_qt/dialogs/new_style_dialog.py). Saves the style on Save; saved() is it once accepted.
// Affinity: GUI thread.
class NewStyleDialog : public QDialog {
    Q_OBJECT
public:
    // `settings` is where the style is saved; it must outlive the dialog.
    explicit NewStyleDialog(SettingsStore& settings, QWidget* parent = nullptr);

    // The style Save kept; nullopt until then.
    const std::optional<walkthrough::WriteUpStyle>& saved() const { return saved_; }

    QLineEdit* nameEdit() const { return nameEdit_; }
    QPlainTextEdit* promptEdit() const { return promptEdit_; }
    QLabel* error() const { return error_; }
    PrimaryButton* saveButton() const { return saveButton_; }

    // Saves the style and accepts, or says why it can't be saved and stays open (_save).
    void save();

private:
    void build();

    SettingsStore& settings_;
    std::optional<walkthrough::WriteUpStyle> saved_;
    QLineEdit* nameEdit_ = nullptr;
    QPlainTextEdit* promptEdit_ = nullptr;
    QLabel* error_ = nullptr;
    PrimaryButton* saveButton_ = nullptr;
};

} // namespace fv
