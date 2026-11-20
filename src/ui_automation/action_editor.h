#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>

#include <QString>

#include "automation/action_model.h"
#include "automation/action_result.h"
#include "base/json.h"
#include "ui_widgets/setting_tile.h"

class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace fv {

class ActionRelay;
class AgentFields;
class Button;
class Columns;
class NoWheelSpinBox;
class OutputCombo;
class PathField;
class SettingsStore;
class ShapingField;
class ShortcutField;
class WrappedLabel;

// How "Try it" reaches the app: runs `action` on `sample` words and calls `done` once with the
// result, from any thread.
using TryAction = std::function<void(const automation::Action& action, const QString& sample,
                                     std::function<void(automation::ActionResult)> done)>;

// The action editor (ui_qt/widgets/action_editor.py): what a new or saved action does, how its
// words are prepared and how it starts (tileId "actionEditor"). Every kind's fields are made
// once and filled every time, with the kind's defaults where they don't apply, so no box keeps
// the last action's values; the kind decides which show. Nothing is saved until Save, or until
// Settings leaves the page with a valid draft. Save writes, inside SettingsStore::mutate, only
// the edits made since opening onto the action as saved now (ontoSaved), so a change the Agent
// hotkey card saved meanwhile (its switch, say) stays. Answers from the relay (Try it, a scan,
// a signing secret) count only for the request that asked them.
// Affinity: GUI thread.
class ActionEditor : public InfoTile {
    Q_OBJECT
public:
    // `store` and `relay` outlive the editor.
    ActionEditor(SettingsStore& store, ActionRelay& relay, TryAction tryAction,
                 QWidget* parent = nullptr);

    // onto_saved: `draft`'s edits since `baseline`, made to `saved`, the action as it is saved
    // now. Every field but params that differs from the baseline, and every parameter that is
    // new or changed; the rest stays as saved.
    static automation::Action ontoSaved(const automation::Action& saved,
                                        const automation::Action& baseline,
                                        const automation::Action& draft);

    // "" when nothing is open.
    QString actionId() const;
    bool isNew() const { return isNew_; }
    // The action as saved when the editor last read it; nullopt for a new one (or none open).
    std::optional<automation::Action> savedAction() const;

    // Shows `action`; a new one is only saved once it is saved. A new action replaced unsaved
    // forgets the signing secret it made.
    void open(const automation::Action& action, bool isNew, const QString& note = {});
    // `saved` changed elsewhere (the Agent hotkey card): shows it, keeping the edits made here.
    void follow(const automation::Action& saved);
    // Profiles, transforms and app awareness may have changed on other pages.
    void refreshChoices(const Json& settings);

    // The action as the fields now say; only the parameters of its kind (PARAMS), in their order.
    // Only while an action is open.
    automation::Action draft() const;
    bool hasUnsavedChanges() const;

    void cancelCapture();
    // Copies this action's signing secret, making one the first time.
    void copySecret();
    // Replaces the signing secret (asking first for a saved action) and copies the new one.
    void newSecret();
    void tryIt();
    // Saves the draft; the reason it can't be saved shows above the buttons. True when saved
    // (or nothing is open).
    bool save();
    // Lets the draft go, then cancelled().
    void cancel();
    // Lets the draft go unsaved, and a new action's signing secret with it.
    void discard();
    // Saves an unsaved edit when Settings leaves the page. With `ask`, a draft that can't be
    // saved asks whether to discard it; false when the person chose to keep editing.
    bool saveDraft(bool ask = false);
    // The line above the buttons, in `tone` ("error" unless given).
    void showMessage(const QString& text, const QString& tone = QStringLiteral("error"));

    QLineEdit* nameEdit() const { return nameEdit_; }
    QLineEdit* templateEdit() const { return templateEdit_; }
    WrappedLabel* preview() const { return preview_; }
    WrappedLabel* presetNote() const { return presetNote_; }
    QWidget* linkBox() const { return linkBox_; }
    AgentFields* agentFields() const { return agentFields_; }
    QWidget* agentBox() const { return agentBox_; }
    QLineEdit* urlEdit() const { return urlEdit_; }
    WrappedLabel* urlNote() const { return urlNote_; }
    OutputCombo* replyCombo() const { return replyCombo_; }
    Button* copySecretButton() const { return copySecretButton_; }
    Button* newSecretButton() const { return newSecretButton_; }
    WrappedLabel* secretHint() const { return secretHint_; }
    QWidget* webhookBox() const { return webhookBox_; }
    PathField* programField() const { return programField_; }
    QPlainTextEdit* argsEdit() const { return argsEdit_; }
    OutputCombo* programOutput() const { return programOutput_; }
    NoWheelSpinBox* timeoutSpin() const { return timeoutSpin_; }
    QWidget* programBox() const { return programBox_; }
    WrappedLabel* kindNote() const { return kindNote_; }
    QPushButton* mcpLink() const { return mcpLink_; }
    QWidget* noteBox() const { return noteBox_; }
    ShapingField* shaping() const { return shaping_; }
    ShortcutField* shortcut() const { return shortcut_; }
    QLineEdit* spokenEdit() const { return spokenEdit_; }
    QLineEdit* descriptionEdit() const { return descriptionEdit_; }
    Columns* startColumns() const { return startColumns_; }
    QLineEdit* sampleEdit() const { return sampleEdit_; }
    Button* tryButton() const { return tryButton_; }
    WrappedLabel* tryResult() const { return tryResult_; }
    WrappedLabel* message() const { return message_; }
    Button* cancelButton() const { return cancelButton_; }
    Button* saveButton() const { return saveButton_; }

Q_SIGNALS:
    void saved(const fv::automation::Action& action);
    void cancelled();
    void captureChanged(bool capturing);
    // A link to another Settings page: "styles" or "mcp".
    void destinationRequested(const QString& key);

private:
    Json settingsNow() const;
    void fill(const automation::Action& action);
    void showAgents(const std::map<std::string, bool>& found);
    void onScanDone(int token);
    void updatePreview();
    void updateUrlNote();
    void onShortcutCaptured(const QString& hotkey);
    void fetchSecret(bool renew);
    void setSecretBusy(bool busy);
    void onSecretDone(int token, const QString& secret, const QString& error);
    void dropUnsavedSecret();
    void onTryDone(int token, const automation::ActionResult& result);
    void showTryResult(const QString& text, const QString& tone);

    SettingsStore& store_;
    ActionRelay& relay_;
    TryAction tryAction_;
    std::optional<automation::Action> baseline_;
    std::optional<automation::Action> action_;
    bool isNew_ = false;
    int tryToken_ = 0;
    int scanToken_ = 0;
    bool agentsScanned_ = false;
    int secretToken_ = 0;
    bool secretRenew_ = false;
    bool secretMade_ = false;

    QLineEdit* nameEdit_;
    QLineEdit* templateEdit_;
    WrappedLabel* templateHint_;
    WrappedLabel* preview_;
    WrappedLabel* presetNote_;
    QWidget* linkBox_;
    AgentFields* agentFields_;
    QWidget* agentBox_;
    QLineEdit* urlEdit_;
    WrappedLabel* urlNote_;
    OutputCombo* replyCombo_;
    Button* copySecretButton_;
    Button* newSecretButton_;
    WrappedLabel* secretHint_;
    QWidget* webhookBox_;
    PathField* programField_;
    QPlainTextEdit* argsEdit_;
    OutputCombo* programOutput_;
    NoWheelSpinBox* timeoutSpin_;
    QWidget* programBox_;
    WrappedLabel* kindNote_;
    QPushButton* mcpLink_;
    QWidget* noteBox_;
    ShapingField* shaping_;
    ShortcutField* shortcut_;
    QLineEdit* spokenEdit_;
    QLineEdit* descriptionEdit_;
    Columns* startColumns_;
    QLineEdit* sampleEdit_;
    Button* tryButton_;
    WrappedLabel* tryResult_;
    WrappedLabel* message_;
    Button* cancelButton_;
    Button* saveButton_;
};

} // namespace fv
