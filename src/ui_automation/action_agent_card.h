#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QString>

#include "automation/action_model.h"
#include "automation/action_result.h"
#include "base/json.h"
#include "ui_automation/action_editor.h"
#include "ui_widgets/setting_tile.h"

class QPushButton;

namespace fv {

class ActionRelay;
class AgentFields;
class Button;
class Reflow;
class SettingsStore;
class SettingsSwitch;
class ShortcutField;
class WrappedLabel;

namespace action_agent_card {

inline constexpr std::string_view TITLE = "Agent hotkey";
inline constexpr std::string_view INVITE =
    "Press a shortcut, say what you need, and your coding agent takes it from there.";
inline constexpr std::string_view LOOKING = "Looking for Claude Code, Codex or OpenCode…";
inline constexpr std::string_view NONE_FOUND = "None found — install one, or use another action.";
// What "Try it" sends: harmless in a session and cheap as a quick answer.
inline constexpr std::string_view AGENT_SAMPLE = "Say hello in one short sentence.";

// The spoken name and the purpose a new agent action starts with, and follows the agent with
// while they are unchanged (_spoken_for, _description_for).
std::vector<std::string> spoken_for(std::string_view agent_id);
std::string description_for(std::string_view agent_id);

} // namespace action_agent_card

// The featured "Agent hotkey" card at the top of Settings > Automate > Actions
// (ui_qt/widgets/action_agent_card.py, tileId "actionAgentCard"). It creates or edits the action
// with id AGENT_ACTION_ID: press a shortcut, say a task, and Claude Code, Codex or OpenCode picks
// it up. Every choice saves as it is made; only the agents a scan found can be chosen. The scan
// is asked for once, when the card is first shown and nothing was scanned yet; the last scan is
// read through agents::available() and never blocks.
// Affinity: GUI thread.
class AgentHotkeyCard : public InfoTile {
    Q_OBJECT
public:
    // `store` and `relay` outlive the card.
    AgentHotkeyCard(SettingsStore& store, ActionRelay& relay, TryAction tryAction,
                    QWidget* parent = nullptr);

    // The saved agent action, or nullopt before it is set up.
    const std::optional<automation::Action>& action() const { return action_; }

    // Shows the saved agent action, or the invitation to set one up; nothing while its shortcut
    // field is capturing.
    void load(const Json& settings);
    // Asks for a scan once, when agents::available() knows nothing yet.
    void ensureScanned();
    void startScan(bool refresh);
    // Creates the agent action with the chosen agent, then asks for its shortcut.
    void setUp();
    // Asks first; then the shortcut goes too and the agent stays picked for a new set-up.
    void remove();
    void cancelCapture();
    void tryIt();
    void showMessage(const QString& text, const QString& tone);

    SettingsSwitch* enabledSwitch() const { return enabledSwitch_; }
    AgentFields* fields() const { return fields_; }
    WrappedLabel* status() const { return status_; }
    QPushButton* lookAgain() const { return lookAgain_; }
    Reflow* statusRow() const { return statusRow_; }
    Button* setupButton() const { return setupButton_; }
    QWidget* setupRow() const { return setupRow_; }
    QWidget* shortcutBox() const { return shortcutBox_; }
    ShortcutField* shortcut() const { return shortcut_; }
    WrappedLabel* message() const { return message_; }
    Button* tryButton() const { return tryButton_; }
    Button* moreButton() const { return moreButton_; }
    QPushButton* removeButton() const { return removeButton_; }
    Reflow* footer() const { return footer_; }

Q_SIGNALS:
    // The saved action changed (created, edited, switched, removed).
    void changed();
    void captureChanged(bool capturing);
    void stylesRequested();
    // "Customize": open the full editor on the agent action.
    void editRequested(const fv::automation::Action& action);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void render();
    QString unsetAgent() const;
    void syncStatus();
    void onScanDone(int token);
    void showAvailable(const std::map<std::string, bool>& available, bool animate);
    bool save(const automation::Action& action);
    void onFieldsChanged();
    void onShortcutCaptured(const QString& hotkey);
    void onEnabledToggled(bool on);
    void onTryDone(int token, const automation::ActionResult& result);

    SettingsStore& store_;
    ActionRelay& relay_;
    TryAction tryAction_;
    std::optional<automation::Action> action_;
    // The agent picked before setting it up; it survives a remove.
    QString picked_;
    int scanToken_ = 0;
    int tryToken_ = 0;
    bool scanned_ = false;

    SettingsSwitch* enabledSwitch_;
    AgentFields* fields_;
    WrappedLabel* status_;
    QPushButton* lookAgain_;
    Reflow* statusRow_;
    Button* setupButton_;
    QWidget* setupRow_;
    QWidget* shortcutBox_;
    ShortcutField* shortcut_;
    WrappedLabel* message_;
    Button* tryButton_;
    Button* moreButton_;
    QPushButton* removeButton_;
    Reflow* footer_;
};

} // namespace fv
