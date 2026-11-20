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
#include "text/cleanup_profiles.h"

class QBoxLayout;
class QCheckBox;
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

// The library and editor of on-demand cleanup profiles on Settings > Personalize > Profiles
// (ui_qt/widgets/cleanup_profiles_panel.py CleanupProfilesPanel, object name
// "cleanupProfilesPanel"): named output formats, each with optional recording shortcut, saved
// through the settings file's mutate. Profiles always run AI cleanup with the configured text
// model, whatever the cleanup switch says.
//
// An unsaved edit is never dropped silently: switching profiles asks Save, Discard or Cancel
// (Save the default), refresh() keeps the draft while still taking a shortcut saved elsewhere
// (the Hotkeys page) unless the draft changed it, and Settings calls saveDraft when the page is
// left. A shortcut already in use is refused on save, with the reason, and the draft stays. The
// editor goes under the library when the panel is narrower than the library, the 18 px gap and
// the editor's minimum width. Instructions are the user's own text: never log them.
// Affinity: GUI thread.
class CleanupProfilesPanel : public QWidget {
    Q_OBJECT
public:
    // Wide enough for Duplicate and Delete to sit side by side at every font scale.
    static constexpr int kLibraryWidth = 210;
    // A profile name's longest length, in code points.
    static constexpr int kNameMaxChars = 80;

    // QMessageBox::question's signature: the seam a test answers instead of the user.
    using Question = std::function<QMessageBox::StandardButton(
        QWidget* parent, const QString& title, const QString& text,
        QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton)>;

    // `store` (OpenWhisper's settings manager) is the file the library is read from and saved
    // to; it outlives the panel.
    explicit CleanupProfilesPanel(SettingsStore& store, QWidget* parent = nullptr);
    ~CleanupProfilesPanel() override;

    // The saved actions' shortcuts, which the shortcut check counts as taken; fv_text cannot
    // load them. None until set.
    void setActionBindings(std::function<std::vector<hotkeys::Binding>()> bindings);

    bool hasUnsavedChanges() const;
    // Saves an unsaved edit when Settings is left, as its other editors do. A draft that can't
    // be saved yet stays in the editor with the reason; with `ask` the user discards it or keeps
    // editing instead. False when the user chose to keep editing.
    bool saveDraft(bool ask = false);
    // Re-reads the library and shows `selectedId` ("" the first profile); without one, an
    // unsaved draft is kept and only follows a shortcut saved elsewhere.
    void refresh(std::optional<std::string> selectedId = std::nullopt);
    void newProfile();
    // Starts an unsaved copy of the draft named "<first 65 characters> copy" (then " copy 2",
    // ...), without its shortcut.
    void duplicateProfile();
    // Fills the instructions from a starter (asking first when some are already written) and
    // the name when it is blank.
    void useTemplate(const text::CleanupProfile& profile);
    bool saveProfile();
    // Asks, then deletes the shown profile.
    void deleteProfile();
    void cancelCapture();

    // QMessageBox::question unless a test replaces it.
    Question question;

    QListWidget* profileList() const { return list_; }
    Button* newButton() const { return newButton_; }
    Button* duplicateButton() const { return duplicateButton_; }
    Button* deleteButton() const { return deleteButton_; }
    QLineEdit* nameEdit() const { return nameEdit_; }
    QTextEdit* instructionsEdit() const { return instructionsEdit_; }
    ButtonRow* templateRow() const { return templateRow_; }
    QCheckBox* rulesCheck() const { return rulesCheck_; }
    ProfileHotkeyInput* hotkeyInput() const { return hotkeyInput_; }
    WrappedLabel* message() const { return message_; }
    Button* modelButton() const { return modelButton_; }
    Button* saveButton() const { return saveButton_; }
    QBoxLayout* columns() const { return columns_; }
    QWidget* library() const { return library_; }
    QWidget* editor() const { return editor_; }

Q_SIGNALS:
    void profilesChanged();
    void captureChanged(bool capturing);
    // "Choose cleanup model…": the AI cleanup page's model section.
    void modelRequested();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    Json settings() const;
    text::CleanupProfile draft() const;
    bool saveBeforeSwitch();
    void followSavedHotkey();
    void showProfile(const text::CleanupProfile& profile);
    void startDraft(const text::CleanupProfile& profile);
    void selectionChanged(QListWidgetItem* current, QListWidgetItem* previous);

    SettingsStore& store_;
    std::function<std::vector<hotkeys::Binding>()> actions_;
    std::string profileId_;
    std::optional<text::CleanupProfile> saved_;
    bool loading_ = false;

    QBoxLayout* columns_ = nullptr;
    QWidget* library_ = nullptr;
    QWidget* editor_ = nullptr;
    QListWidget* list_ = nullptr;
    Button* newButton_ = nullptr;
    Button* duplicateButton_ = nullptr;
    Button* deleteButton_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QTextEdit* instructionsEdit_ = nullptr;
    ButtonRow* templateRow_ = nullptr;
    QCheckBox* rulesCheck_ = nullptr;
    ProfileHotkeyInput* hotkeyInput_ = nullptr;
    WrappedLabel* message_ = nullptr;
    Button* modelButton_ = nullptr;
    Button* saveButton_ = nullptr;
};

} // namespace fv
