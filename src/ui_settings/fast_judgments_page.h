#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QPointer>
#include <QString>
#include <QWidget>

#include "appkit/qt_bridge.h"
#include "appkit/settings_page.h"
#include "base/json.h"
#include "llm/judge.h"
#include "ui_settings/typesafe_credential.h"

class QVBoxLayout;

namespace fv {

class AppContext;
class Button;
class Executor;
class InfoTile;
class SegmentedBar;
class SettingsDialog;
class SettingsPage;
class SettingTile;
class WrappedLabel;

namespace detail {
template <class Widget> class Mailbox;
}

// The words and choices of Settings → Meeting Mode → Fast judgments
// (ui_qt/dialogs/settings_fast_judgments.py), without a widget: what the route and the keys say
// the page shows. Affinity: thread-safe (pure functions).
namespace fast_judgments {

// The routes in the Judge bar's order (TypeSafeProvider.ALL) and the text models in theirs
// (TypeSafeTextModel.ALL).
inline constexpr std::string_view kRouteIds[] = {"typesafe", "openrouter", "text_model"};
inline constexpr std::string_view kTextModelIds[] = {"meeting", "cleanup"};

// The three routes the settings name: TypeSafe, OpenRouter and the text model they point at.
std::vector<llm::JudgeRoute> routes(const Json& settings);
// _other_route: the other way to Jev, whose key can stand in; nullopt for a text model.
std::optional<llm::JudgeRoute> other_route(const llm::JudgeRoute& route);
// _destination_copy: who receives excerpts over `route`; OpenRouter is a hop, not the model.
std::string destination_copy(const llm::JudgeRoute& route);
// _text_model_label: "<name> · <model after the last '/'>", or the name alone.
std::string text_model_label(const llm::JudgeRoute& route);
// rail_value: "Off", "No key", "On · no features" or "On · N", plus " · <route>" off TypeSafe.
std::string rail_value(const Json& settings, bool key_present);

// What the Judge tile shows for the settings (_render_route). A route's key state is asked of
// `key_present`.
struct RouteView {
    int route_index = 0;
    std::vector<std::string> route_details;
    bool text_model_visible = false;
    int text_model_index = 0;
    std::vector<std::string> text_model_details;
    std::string description;
    std::string destination;
};
RouteView route_view(const Json& settings,
                     const std::function<bool(const llm::JudgeRoute&)>& key_present);

// What the key notice shows (render_key_state) for the chosen `route`, whether its key resolves
// and whether the other Jev route's does.
struct KeyNotice {
    bool visible = false;
    // Unset while a key is present: the notice keeps what it last said.
    std::optional<std::string> title;
    std::optional<std::string> description;
    // The "Use <other>" button: shown only when the other route has a key and this one does not.
    bool switch_visible = false;
    // Unset for a text model, which has no other route.
    std::optional<std::string> switch_text;
    // "neutral" while Use-other leads, else "primary".
    std::string add_tone;
    std::string add_tooltip;
};
KeyNotice key_notice(const llm::JudgeRoute& route, bool key_present, bool other_key_present,
                     bool master_on);

} // namespace fast_judgments

// Which routes have a key, read off the GUI thread (the credential store blocks) and kept for
// the rail and the page, which redraw on every save. A key is looked up by its variable; a route
// that takes none always has one. One not read yet counts as not set, as the API keys rail does.
// Affinity: GUI thread; reads run on the worker.
class FastJudgmentsState {
public:
    // `keys` must outlive the state; `ui` runs the results on the GUI thread.
    static std::shared_ptr<FastJudgmentsState> create(std::shared_ptr<JudgeKeys> keys,
                                                      Executor& worker, Executor& ui);

    // Reads the routes' keys on the worker; `done` runs on the GUI thread afterwards. Reads may
    // finish out of order: a newer read's answer for a variable wins.
    void read(std::vector<llm::JudgeRoute> routes, std::function<void()> done = {});
    bool present(const llm::JudgeRoute& route) const;
    // Whether every route's key has been read, and whether a read of one is on its way.
    bool knows(const std::vector<llm::JudgeRoute>& routes) const;
    bool reading(const std::vector<llm::JudgeRoute>& routes) const;

    const std::shared_ptr<JudgeKeys>& keys() const { return keys_; }
    Executor& worker() const { return worker_; }

private:
    FastJudgmentsState(std::shared_ptr<JudgeKeys> keys, Executor& worker, Executor& ui);

    std::shared_ptr<JudgeKeys> keys_;
    Executor& worker_;
    Executor& ui_;
    std::weak_ptr<FastJudgmentsState> self_;
    std::uint64_t requested_ = 0;
    std::map<std::string, bool, std::less<>> present_;
    std::map<std::string, std::uint64_t, std::less<>> stamps_;
    std::map<std::string, int, std::less<>> pending_;
};

// Settings → Meeting Mode → Fast judgments (FastJudgmentsSection): the key notice, the Judge tile
// (TypeSafe, OpenRouter or one of the user's own text models, with a Test button) and the
// Features group with the master switch and the five feature switches. Says plainly when the key
// that powers the judge is missing, since the features degrade to "no judgment" on purpose.
// Keys are read on the worker; a route test runs there too, and its answer comes back through a
// TokenGate on this page, so a closed page is not held by the worker.
// Affinity: GUI thread.
class FastJudgmentsPage : public QWidget, public SettingsPageModule {
    Q_OBJECT
public:
    FastJudgmentsPage(SettingsPage& page, SettingsDialog& dialog,
                      std::shared_ptr<FastJudgmentsState> state);
    ~FastJudgmentsPage() override;

    // The section's controls (CONTROLS).
    InfoTile* keyNotice() const { return keyNotice_; }
    Button* openKeyButton() const { return openKeyButton_; }
    Button* switchRouteButton() const { return switchRouteButton_; }
    InfoTile* routeTile() const { return routeTile_; }
    SegmentedBar* routeBar() const { return routeBar_; }
    SegmentedBar* textModelBar() const { return textModelBar_; }
    Button* routeTestButton() const { return routeTestButton_; }
    WrappedLabel* routeStatus() const { return routeStatus_; }
    SettingTile* enabledTile() const { return enabledTile_; }
    SettingTile* topicShiftTile() const { return topicShiftTile_; }
    SettingTile* voiceCommandsTile() const { return voiceCommandsTile_; }
    // The tiles of the other four features, by key ("citations", "semantic_search",
    // "question_radar", "highlights"), in the page's order.
    const std::vector<std::pair<std::string, SettingTile*>>& featureTiles() const {
        return featureTiles_;
    }
    WrappedLabel* destinationLabel() const { return destinationLabel_; }

    // selected_route: TypeSafe unless the settings chose another.
    llm::JudgeRoute selectedRoute() const;
    // rail_value for the settings as the window holds them now.
    QString railValue() const;
    // A route test is running.
    bool testing() const { return testing_; }
    // Whether every key the page shows has been read and none is being read.
    bool stateCurrent() const;

    // test_route: sends one judgment over the chosen route, or says there is no key to test
    // (at once when the last read, the environment and .env have none; after the store's
    // answer while the keys are unread or being read again).
    void testRoute();
    // render_key_state: the Judge tile, then the key notice.
    void renderKeyState();
    // update_feature_tiles: the feature switches only mean something while the master is on.
    void updateFeatureTiles();

    // SettingsPageModule.
    void loadSettings(const Json& settings) override;
    void refreshPage() override;
    QWidget* namedControl(std::string_view name) override;

    // The names the page's controls answer to (the dialog's CONTROL_DESTINATIONS for it).
    static const std::vector<std::string>& controlNames();

private:
    void build(SettingsPage& page, QVBoxLayout* layout);
    void renderRoute();
    void showRouteResult(std::optional<bool> ok, const QString& text);
    void onEnabledChanged(bool checked);
    void onRouteActivated(int index);
    void onTextModelActivated(int index);
    void switchRoute();
    void onRouteVerified(const llm::JudgeRoute& tested, std::optional<llm::VerifyResult> result);
    void onRouteKeyRead(const llm::JudgeRoute& route, bool present);
    void startRouteTest(const llm::JudgeRoute& route);
    static QString noKeyText(const llm::JudgeRoute& route);
    Json settings() const;

    SettingsDialog& dialog_;
    std::shared_ptr<FastJudgmentsState> state_;
    qt::TokenGate testGate_;
    std::shared_ptr<detail::Mailbox<FastJudgmentsPage>> mailbox_;

    InfoTile* keyNotice_ = nullptr;
    Button* openKeyButton_ = nullptr;
    Button* switchRouteButton_ = nullptr;
    InfoTile* routeTile_ = nullptr;
    SegmentedBar* routeBar_ = nullptr;
    SegmentedBar* textModelBar_ = nullptr;
    Button* routeTestButton_ = nullptr;
    WrappedLabel* routeStatus_ = nullptr;
    SettingTile* enabledTile_ = nullptr;
    SettingTile* topicShiftTile_ = nullptr;
    SettingTile* voiceCommandsTile_ = nullptr;
    std::vector<std::pair<std::string, SettingTile*>> featureTiles_;
    WrappedLabel* destinationLabel_ = nullptr;

    bool testing_ = false;
};

// What registerFastJudgmentsPage reads from the context unless given.
struct FastJudgmentsOptions {
    // Where the routes' keys are looked up and verified; JudgeKeys::over(context) when unset. It
    // is also provided to the context, where the API keys page finds it for TypeSafe's Test.
    std::shared_ptr<JudgeKeys> keys;
    // Where the page's blocking work runs: ctx.io().
    Executor* worker = nullptr;
    // The Settings window whose rail a background key read redraws: SettingsWindow::dialog().
    std::function<SettingsDialog*()> dialog;
};

// Registers destinations::meeting_fast with its rail value, search fields and control names,
// provides the JudgeKeys to the context when it has none, and follows the API keys page: a key
// saved or removed there (SettingsPagesSignals::apiKeysChanged, when the context provides it)
// reads the keys again.
void registerFastJudgmentsPage(AppContext& context, FastJudgmentsOptions options = {});

} // namespace fv
