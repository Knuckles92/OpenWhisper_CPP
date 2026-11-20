#pragma once

#include <string>
#include <string_view>

#include <QPointer>
#include <QString>
#include <QWidget>

#include "automation/action_model.h"
#include "base/json.h"
#include "ui_automation/action_editor.h"

class QLabel;
class QTimer;

namespace fv {

class ActionList;
class ActionRelay;
class Button;
class PresetGallery;
class Reflow;
class SettingsStore;
class WrappedLabel;

namespace action_panel {

// How long a just-saved card glows (ms).
inline constexpr int kFlashMs = 1600;
// Room kept above and below a part of the page scrolled into view (px).
inline constexpr int kRevealMargin = 24;

// unique_name: `name`, or "name 2", "name 3"… when an action already has it (case-folded).
std::string unique_name(std::string_view name, const Json& settings);
// saved_note: what the panel says once `action` is saved, by how it starts.
std::string saved_note(const automation::Action& action);

} // namespace action_panel

// Your actions (ui_qt/widgets/action_panel.py): the cards, the Add action gallery and the editor,
// one at a time (tileId "actionsPanel"). Saves through SettingsStore::mutate; a change it saves
// is announced by actionsChanged(). A target it opens or saves is scrolled into view inside the
// enclosing scroll area once the event loop has laid it out.
// Affinity: GUI thread.
class ActionsPanel : public QWidget {
    Q_OBJECT
public:
    enum class Mode { List, Gallery, Editor };

    // `store` and `relay` outlive the panel.
    ActionsPanel(SettingsStore& store, ActionRelay& relay, TryAction tryAction,
                 QWidget* parent = nullptr);

    Mode mode() const { return mode_; }
    // "list", "gallery" or "editor".
    QString modeName() const;

    QLabel* title() const { return title_; }
    Button* addButton() const { return addButton_; }
    Reflow* header() const { return header_; }
    WrappedLabel* notice() const { return notice_; }
    ActionList* list() const { return list_; }
    PresetGallery* gallery() const { return gallery_; }
    ActionEditor* editor() const { return editor_; }

    // Shows what `settings` hold (the store's when null): the cards, and in the editor its
    // choices and the edited action as saved now.
    void refresh();
    void refresh(const Json& settings);
    void showList();
    // Opens the gallery.
    void addAction();
    void startFromPreset(const automation::Preset& preset);
    // Opens the editor on a saved action, first asking about unsaved edits of another.
    void edit(const QString& actionId);
    void setActionEnabled(const QString& actionId, bool on);
    // Deletes after asking; a webhook's signing secret is forgotten off the GUI thread.
    void deleteAction(const QString& actionId);
    bool saveDraft(bool ask = false);
    void cancelCapture();
    // The line under the header, in `tone` ("ok", "error" or none); "" hides it.
    void showNotice(const QString& text, const QString& tone = {});

Q_SIGNALS:
    void actionsChanged();
    void captureChanged(bool capturing);
    void destinationRequested(const QString& key);
    // The page came into view; another page may have changed what it shows.
    void shown();

protected:
    void showEvent(QShowEvent* event) override;

private:
    Json settingsNow() const;
    void followSaved(const Json& settings);
    void showMode(Mode mode);
    bool leaveEditor();
    void openEditor(const automation::Action& action, bool isNew, const QString& note = {});
    void onSaved(const automation::Action& action);
    void flash(const QString& actionId);
    void endFlash();
    void scrollTo(QWidget* widget);
    void reveal();

    SettingsStore& store_;
    ActionRelay& relay_;
    Mode mode_ = Mode::List;
    QLabel* title_;
    Button* addButton_;
    Reflow* header_;
    WrappedLabel* notice_;
    ActionList* list_;
    PresetGallery* gallery_;
    ActionEditor* editor_;
    QString flashCardId_;
    QTimer* flashTimer_;
    QPointer<QWidget> revealTarget_;
    QTimer* revealTimer_;
};

} // namespace fv
