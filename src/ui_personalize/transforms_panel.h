#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <QMessageBox>
#include <QString>
#include <QWidget>

#include "base/json.h"
#include "hotkeys/conflicts.h"
#include "text/transforms.h"

class QBoxLayout;
class QListWidget;
class QListWidgetItem;
class QLineEdit;
class QTextEdit;

namespace fv {

class Button;
class ButtonRow;
class ProfileHotkeyInput;
class SettingsStore;
class WrappedLabel;

// The transforms library and editor on Settings > Commands (ui_qt/widgets/command_settings.py
// TransformsPanel, object name "transformsPanel"): saved rewrites for selected text, each with an
// optional shortcut, saved through the settings file's mutate.
//
// An unsaved edit is never dropped silently: switching transforms asks Save, Discard or Cancel
// (Save the default), refresh() keeps the draft while still taking a shortcut saved elsewhere
// (the Hotkeys page) unless the draft changed it, and Settings calls saveDraft when the page is
// left. A captured shortcut another binding already uses goes back to the one held before the
// capture, with the reason. The editor goes under the library when the panel is narrower than
// the library, the 18 px gap and the editor's minimum width. Instructions are the user's own
// text: never log them.
// Affinity: GUI thread.
class TransformsPanel : public QWidget {
    Q_OBJECT
public:
    // The library's width beside the editor, unless its buttons need more.
    static constexpr int kLibraryWidth = 210;

    // QMessageBox::question's signature: the seam a test answers instead of the user.
    using Question = std::function<QMessageBox::StandardButton(
        QWidget* parent, const QString& title, const QString& text,
        QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton)>;

    // `store` (OpenWhisper's settings manager) is the file the library is read from and saved
    // to; it outlives the panel.
    explicit TransformsPanel(SettingsStore& store, QWidget* parent = nullptr);
    ~TransformsPanel() override;

    // The saved actions' shortcuts, which the shortcut checks count as taken; fv_text cannot
    // load them. None until set.
    void setActionBindings(std::function<std::vector<hotkeys::Binding>()> bindings);

    bool hasUnsavedChanges() const;
    // Saves an unsaved edit when Settings leaves the page. A draft that can't be saved yet stays
    // in the editor with the reason; with `ask` the user discards it or keeps editing instead.
    // False when the user chose to keep editing.
    bool saveDraft(bool ask = false);
    // Re-reads the library and shows `selectedId` ("" the first transform); without one, an
    // unsaved draft is kept and only follows a shortcut saved elsewhere.
    void refresh(std::optional<std::string> selectedId = std::nullopt);
    void newTransform();
    // Starts an unsaved copy of the draft named "<name> copy" (then " copy 2", ...).
    void duplicateTransform();
    // Fills the instruction from a starter (asking first when one is already written) and the
    // name when it is blank.
    void useStarter(const text::Transform& starter);
    bool saveTransform();
    // Asks, then deletes the shown transform.
    void deleteTransform();
    void cancelCapture();

    // QMessageBox::question unless a test replaces it.
    Question question;

    QListWidget* transformList() const { return list_; }
    Button* newButton() const { return newButton_; }
    Button* duplicateButton() const { return duplicateButton_; }
    Button* deleteButton() const { return deleteButton_; }
    QLineEdit* nameEdit() const { return nameEdit_; }
    QTextEdit* instructionEdit() const { return instructionEdit_; }
    ButtonRow* starterRow() const { return starterRow_; }
    ProfileHotkeyInput* hotkeyInput() const { return hotkeyInput_; }
    WrappedLabel* message() const { return message_; }
    Button* saveButton() const { return saveButton_; }
    QBoxLayout* columns() const { return columns_; }
    QWidget* library() const { return library_; }
    QWidget* editor() const { return editor_; }

Q_SIGNALS:
    void transformsChanged();
    void captureChanged(bool capturing);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    int libraryWidth() const;
    Json settings() const;
    text::ShortcutContext shortcutContext() const;
    text::Transform draft() const;
    bool saveBeforeSwitch();
    void followSavedHotkey();
    void showTransform(const text::Transform& transform);
    void startDraft(const text::Transform& transform);
    void selectionChanged(QListWidgetItem* current, QListWidgetItem* previous);
    void onCaptureChanged(bool capturing);
    void onHotkeyCaptured(const QString& hotkey);

    SettingsStore& store_;
    std::function<std::vector<hotkeys::Binding>()> actions_;
    std::string transformId_;
    std::optional<text::Transform> saved_;
    bool loading_ = false;
    QString hotkeyBeforeCapture_;

    QBoxLayout* columns_ = nullptr;
    QWidget* library_ = nullptr;
    QWidget* editor_ = nullptr;
    QListWidget* list_ = nullptr;
    Button* newButton_ = nullptr;
    Button* duplicateButton_ = nullptr;
    Button* deleteButton_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QTextEdit* instructionEdit_ = nullptr;
    ButtonRow* starterRow_ = nullptr;
    ProfileHotkeyInput* hotkeyInput_ = nullptr;
    WrappedLabel* message_ = nullptr;
    Button* saveButton_ = nullptr;
};

} // namespace fv
