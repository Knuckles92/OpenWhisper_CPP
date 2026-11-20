#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <QPointer>
#include <QString>
#include <QWidget>

#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "appkit/settings_page.h"
#include "automation/action_model.h"
#include "automation/action_result.h"
#include "automation/routing.h"
#include "base/json.h"

class QVBoxLayout;

namespace fv {

class ActionRelay;
class ActionsPanel;
class AgentHotkeyCard;
class AppContext;
class CredentialStore;
class Executor;
class InfoTile;
class RouteShortcutField;
class SettingTile;
class SettingsPage;
class SettingsPageHost;
class StatusLine;
class WrappedLabel;
struct ActionRelayWork;

namespace llm {
class JudgeProvider;
}

// What Settings > Automate > Actions says about itself (ui_qt/dialogs/settings_actions.py):
// TITLE, SUBTITLE, SEARCH_FIELDS, CONTROL_ATTRS and rail_value.
namespace actions_page {

inline constexpr std::string_view TITLE = "Actions";
inline constexpr std::string_view SUBTITLE =
    "Send what you say somewhere other than the cursor: a chat, your agent, or your own tools.";
// What Route by voice sends, by the ways it can be used.
inline constexpr std::string_view PRIVACY_SHORTCUT =
    "Everything you record with the Route by voice shortcut is given to the Fast judgments judge "
    "to choose the action.";
inline constexpr std::string_view PRIVACY_SHORTCUT_AND_DICTATION =
    "Everything you record with the Route by voice shortcut, and normal dictations that start "
    "with an action's name, are given to the Fast judgments judge to choose the action.";

struct SearchField {
    std::string_view target;
    std::string_view title;
    std::string_view description;
};
// SEARCH_FIELDS, in page order.
const std::vector<SearchField>& search_fields();
// CONTROL_ATTRS: the search fields' targets, then the status line, the privacy line and the
// Route by voice shortcut.
const std::vector<std::string>& control_names();

// "None yet", "Agent hotkey set", "N action(s)", or both joined with " · ".
std::string rail_value(const Json& settings);

} // namespace actions_page

// The work the session's ActionRelay runs: scans through the installed-agent instance set at the
// time (agents::installed_agents_if_set), signing secrets in `store`, which outlives the relay.
ActionRelayWork sessionRelayWork(CredentialStore& store);

// Settings > Automate > Actions (ui_qt/dialogs/settings_actions.py build, load, refresh,
// save_drafts and cancel_capture): the Agent hotkey card, your actions, and Route by voice's two
// switches, status line, shortcut and privacy line. A page content widget inside the window's
// SettingsPage, with its own column (margins 0, spacing 8). Route by voice's status reads the
// credential store, so it is worked out on the context's io executor and shown when it comes
// back; an answer to an older request is dropped. "Try it" runs through the context's
// automation::ActionTester (absent: it says so), and closing Settings cancels the tries still
// running.
// Affinity: GUI thread.
class ActionsPage : public QWidget, public SettingsPageModule {
    Q_OBJECT
public:
    // automation::status in the app; tests replace it. Called off the GUI thread.
    using RoutingStatusFn =
        std::function<automation::RoutingStatus(const Json& settings, llm::JudgeProvider&)>;
    static RoutingStatusFn& routingStatus();

    // `container` is the page the Settings window made for the destination (the parent); `host`
    // and `context` outlive the page. The relay is the context's ActionRelay (else one of the
    // page's own over the context's io executor and credential store), the judge provider the
    // context's llm::JudgeProvider (else one over the context's HTTP client and credential
    // store). Throws ServiceUnavailable without an io executor.
    ActionsPage(AppContext& context, SettingsPageHost& host, SettingsPage& container);
    ~ActionsPage() override;

    AgentHotkeyCard* agentCard() const { return card_; }
    ActionsPanel* panel() const { return panel_; }
    SettingTile* routingTile() const { return routingTile_; }
    SettingTile* routingDictationTile() const { return routingDictationTile_; }
    StatusLine* routingStatusLine() const { return statusLine_; }
    InfoTile* routeShortcutTile() const { return routeShortcutTile_; }
    RouteShortcutField* routeShortcut() const { return routeShortcut_; }
    WrappedLabel* routingPrivacy() const { return privacy_; }
    QVBoxLayout* pageLayout() const { return layout_; }

    // _try_action: runs `action` through the app; done(ActionResult) later.
    void tryAction(const automation::Action& action, const QString& sample,
                   std::function<void(automation::ActionResult)> done);
    // _cancel_try_runs: cancels the page's Try it runs through the app.
    void cancelTryRuns();
    // refresh(dialog): everything read again from the window's snapshot.
    void refresh();

    // SettingsPageModule.
    void loadSettings(const Json& settings) override;
    void refreshPage() override;
    void cancelCapture() override;
    bool saveDrafts(bool ask) override;
    QWidget* namedControl(std::string_view name) override;

private:
    void showAll(const Json& settings);
    void showRouting(const Json& settings);
    void requestStatus(const Json& settings);
    void applyStatus(const automation::RoutingStatus& status);
    void onRoutingToggled(std::string_view key, bool on);
    void onActionsChanged();
    void onCardChanged();

    AppContext& context_;
    SettingsPageHost& host_;
    Executor& io_;
    std::unique_ptr<ActionRelay> ownRelay_;
    ActionRelay* relay_ = nullptr;
    std::shared_ptr<llm::JudgeProvider> provider_;
    qt::TokenGate statusGate_;

    QVBoxLayout* layout_ = nullptr;
    AgentHotkeyCard* card_ = nullptr;
    ActionsPanel* panel_ = nullptr;
    SettingTile* routingTile_ = nullptr;
    SettingTile* routingDictationTile_ = nullptr;
    StatusLine* statusLine_ = nullptr;
    InfoTile* routeShortcutTile_ = nullptr;
    RouteShortcutField* routeShortcut_ = nullptr;
    WrappedLabel* privacy_ = nullptr;
    std::map<std::string, QPointer<QWidget>, std::less<>> controls_;
};

// Registers the page for destinations::actions in the context's Settings pages, with its rail
// value, search fields and controls. It is built on first selection over a DialogPageHost of the
// Settings window it is built in; building it without one throws std::logic_error.
// Affinity: UI actor.
void registerActionsPage(AppContext& context);

} // namespace fv
