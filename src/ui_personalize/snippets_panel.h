#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QMessageBox>
#include <QString>
#include <QWidget>

#include "base/json.h"
#include "text/snippets.h"

class QBoxLayout;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;

namespace fv {

class Button;
class SettingsStore;
class SettingsSwitch;
class WrappedLabel;

// The snippet library and editor on Settings > Personalize > Snippets
// (ui_qt/widgets/snippets_panel.py SnippetsPanel, object name "snippetsPanel"): pick a snippet on
// the left, edit it on the right; the editor stacks under the library when the panel is narrower
// than the library, the 18 px gap and kEditorMinWidth, all at the font scale.
//
// An unsaved edit is never dropped silently: a draft is dirty while it differs from what is
// saved, switching snippets asks Save, Discard or Cancel (Save the default), refresh() keeps it,
// and Settings calls saveDraft when the page is left. Snippet text and triggers are the user's
// own content: never log them; a failed save or delete logs the exception's type only.
// Affinity: GUI thread.
class SnippetsPanel : public QWidget {
    Q_OBJECT
public:
    // The library column's width at 100 %; Duplicate and Delete fit side by side.
    static constexpr int kLibraryWidth = 220;
    // The narrowest editor column at 100 % before the library moves above it.
    static constexpr int kEditorMinWidth = 300;
    // Offered while the library is empty; each starts a draft with that trigger.
    static constexpr std::array<std::string_view, 3> kSuggestedTriggers{
        "my email address", "my calendar link", "my sign-off"};

    // QMessageBox::question's signature: the seam a test answers instead of the user.
    using Question = std::function<QMessageBox::StandardButton(
        QWidget* parent, const QString& title, const QString& text,
        QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton)>;

    // `store` (OpenWhisper's settings manager) is the file the library is read from and saved
    // to; it outlives the panel.
    explicit SnippetsPanel(SettingsStore& store, QWidget* parent = nullptr);
    ~SnippetsPanel() override;

    bool hasUnsavedChanges() const;
    // Saves an unsaved edit, as Settings saves everything else. A draft that can't be saved yet
    // stays in the editor with the reason; with `ask` the user discards it or keeps editing
    // instead. False when the user chose to keep editing.
    bool saveDraft(bool ask = false);
    // Re-reads the library; keeps an unsaved draft unless told what to select ("" selects the
    // first snippet).
    void refresh(std::optional<std::string> selectedId = std::nullopt);
    // Starts a blank draft, or one with `trigger` filled in; false when the user kept editing.
    bool newSnippet(const QString& trigger = {});
    // Starts an unsaved copy of the draft under the trigger's first 57 characters and the
    // smallest free " N" from 2.
    void duplicateSnippet();
    bool saveSnippet();
    // Asks, then deletes the shown snippet.
    void deleteSnippet();

    // QMessageBox::question unless a test replaces it.
    Question question;

    QLabel* countLabel() const { return countLabel_; }
    QListWidget* snippetList() const { return snippetList_; }
    QFrame* emptyState() const { return emptyState_; }
    const std::vector<Button*>& suggestionButtons() const { return suggestionButtons_; }
    Button* newButton() const { return newButton_; }
    Button* duplicateButton() const { return duplicateButton_; }
    Button* deleteButton() const { return deleteButton_; }
    QLineEdit* triggerEdit() const { return triggerEdit_; }
    WrappedLabel* triggerNote() const { return triggerNote_; }
    QPlainTextEdit* textEdit() const { return textEdit_; }
    SettingsSwitch* formatSwitch() const { return formatSwitch_; }
    WrappedLabel* preview() const { return preview_; }
    Button* saveButton() const { return saveButton_; }
    WrappedLabel* message() const { return message_; }

Q_SIGNALS:
    void snippetsChanged();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void buildLibrary();
    void buildEditor();
    void reflow();
    Json settings() const;
    text::Snippet draft() const;
    bool saveBeforeSwitch();
    text::Snippet snippet(const std::string& snippetId) const;
    // _show: the snippet in the editor; `saved` (else what the editor holds) is what a draft is
    // compared with.
    void showSnippet(const text::Snippet& snippet,
                     const std::optional<text::Snippet>& saved = std::nullopt);
    void updateHints();
    void setMessage(const QString& text, bool error = false);
    void selectionChanged(QListWidgetItem* current, QListWidgetItem* previous);

    SettingsStore& store_;
    std::vector<text::Snippet> snippets_;
    text::Snippet saved_;
    std::string snippetId_;
    bool loading_ = false;

    QLabel* countLabel_ = nullptr;
    QBoxLayout* columns_ = nullptr;
    QWidget* library_ = nullptr;
    QWidget* editor_ = nullptr;
    QListWidget* snippetList_ = nullptr;
    QFrame* emptyState_ = nullptr;
    std::vector<Button*> suggestionButtons_;
    Button* newButton_ = nullptr;
    Button* duplicateButton_ = nullptr;
    Button* deleteButton_ = nullptr;
    QLineEdit* triggerEdit_ = nullptr;
    WrappedLabel* triggerNote_ = nullptr;
    QPlainTextEdit* textEdit_ = nullptr;
    SettingsSwitch* formatSwitch_ = nullptr;
    QBoxLayout* footer_ = nullptr;
    WrappedLabel* preview_ = nullptr;
    Button* saveButton_ = nullptr;
    WrappedLabel* message_ = nullptr;
};

} // namespace fv
