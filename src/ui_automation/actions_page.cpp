#include "ui_automation/actions_page.h"

#include <exception>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <typeinfo>
#include <utility>

#include <QApplication>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "agents/installed_agents.h"
#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "automation/secrets.h"
#include "base/env.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/log.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "http/client.h"
#include "llm/judge.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_automation/action_agent_card.h"
#include "ui_automation/action_fields.h"
#include "ui_automation/action_panel.h"
#include "ui_automation/action_relay.h"
#include "ui_automation/settings_page_host.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_fields.h"
#include "ui_settings/settings_page.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_actions"};

QString q(std::string_view text) {
    return qt::toQString(text);
}

constexpr std::string_view kAgentCopy =
    "Press a shortcut, say what you need, and your coding agent takes it from there.";
constexpr std::string_view kRoutingCopy =
    "Say an action's name first, like “Claude, …”, and your words go to it.";
constexpr std::string_view kOnDictationCopy =
    "A normal dictation that starts with an action's name goes to that action too.";
constexpr std::string_view kRouteShortcutCopy =
    "Record, then send your words to the action they name, or paste them as usual.";

const actions_page::SearchField& field(std::string_view target) {
    for (const actions_page::SearchField& entry : actions_page::search_fields()) {
        if (entry.target == target) {
            return entry;
        }
    }
    throw std::out_of_range("no Actions search field");
}

// The routing status when the judge can't be reached at all: every question to it throws, and
// automation::status says Route by voice isn't available right now.
class UnavailableJudgeProvider final : public llm::JudgeProvider {
public:
    std::shared_ptr<llm::Judge> judge(const Json&) override { throw unavailable(); }
    llm::JudgeRoute route(const Json&) override { throw unavailable(); }
    bool key_present(const Json&) override { throw unavailable(); }

private:
    static std::runtime_error unavailable() {
        return std::runtime_error("No HTTP client or credential store for Fast judgments.");
    }
};

// A judge provider over the context's HTTP client and credential store, for a context nothing
// provided one to. Its environment is built on first use, on the worker that asks.
class ContextJudgeProvider final : public llm::JudgeProvider {
public:
    ContextJudgeProvider(http::Client& http, CredentialStore& store, std::filesystem::path envFile)
        : http_(&http, [](http::Client*) {}), store_(store), dotenv_(std::move(envFile)) {}

    std::shared_ptr<llm::Judge> judge(const Json& settings) override {
        return provider().judge(settings);
    }
    llm::JudgeRoute route(const Json& settings) override { return provider().route(settings); }
    bool key_present(const Json& settings) override { return provider().key_present(settings); }

private:
    llm::JudgeProvider& provider() {
        const std::lock_guard lock(mutex_);
        if (provider_ == nullptr) {
            // The context owns its client; this environment only borrows it.
            environment_ =
                std::make_unique<llm::JudgeEnvironment>(http_, store_, system_env(), dotenv_);
            provider_ = llm::make_judge_provider(*environment_);
        }
        return *provider_;
    }

    std::shared_ptr<http::Client> http_;
    CredentialStore& store_;
    DotEnv dotenv_;
    std::mutex mutex_;
    std::unique_ptr<llm::JudgeEnvironment> environment_;
    std::unique_ptr<llm::JudgeProvider> provider_;
};

std::shared_ptr<llm::JudgeProvider> judgeProvider(AppContext& context) {
    // The process's one provider (the controller provides it); the page shares its ownership.
    if (auto provided = context.find_shared<llm::JudgeProvider>()) {
        return provided;
    }
    try {
        std::filesystem::path envFile;
        try {
            envFile = context.paths().env_file();
        } catch (const ServiceUnavailable&) {
            // No data folder: a missing .env reads as empty.
        }
        return std::make_shared<ContextJudgeProvider>(context.http(), context.credentials(),
                                                      std::move(envFile));
    } catch (const ServiceUnavailable&) {
        return std::make_shared<UnavailableJudgeProvider>();
    }
}

} // namespace

namespace actions_page {

const std::vector<SearchField>& search_fields() {
    static const std::vector<SearchField> fields{
        {"actions_agent_card", "Agent hotkey", kAgentCopy},
        {"actions_panel", "Your actions",
         "Send your words to a chat, a webhook or your own program. Give each a shortcut, or say "
         "its name."},
        {"actions_routing_tile", "Route by voice", kRoutingCopy},
        {"actions_routing_dictation_tile", "Also listen to normal dictations", kOnDictationCopy},
        {"actions_route_shortcut_tile", "Route by voice shortcut", kRouteShortcutCopy},
    };
    return fields;
}

const std::vector<std::string>& control_names() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> made;
        for (const SearchField& entry : search_fields()) {
            made.emplace_back(entry.target);
        }
        for (const char* extra :
             {"actions_routing_status", "actions_routing_privacy", "actions_route_shortcut"}) {
            made.emplace_back(extra);
        }
        return made;
    }();
    return names;
}

std::string rail_value(const Json& settings) {
    const std::vector<automation::Action> actions = automation::load_actions(settings);
    std::size_t count = 0;
    for (const automation::Action& action : actions) {
        if (action.id != automation::AGENT_ACTION_ID) {
            ++count;
        }
    }
    std::string parts;
    if (actions.size() != count) {
        parts = "Agent hotkey set";
    }
    if (count != 0) {
        if (!parts.empty()) {
            parts += " · ";
        }
        parts += std::to_string(count) + (count == 1 ? " action" : " actions");
    }
    return parts.empty() ? std::string("None yet") : parts;
}

} // namespace actions_page

ActionRelayWork sessionRelayWork(CredentialStore& store) {
    ActionRelayWork work;
    work.scanAgents = [](bool refresh) {
        if (const std::shared_ptr<agents::InstalledAgents> agents =
                agents::installed_agents_if_set()) {
            agents->scan_agents(refresh);
        }
    };
    work.ensureSecret = [&store](const std::string& actionId) {
        return automation::ensure_action_secret(store, actionId);
    };
    work.deleteSecret = [&store](const std::string& actionId) {
        automation::delete_action_secret(store, actionId);
    };
    return work;
}

ActionsPage::RoutingStatusFn& ActionsPage::routingStatus() {
    static RoutingStatusFn current = [](const Json& settings, llm::JudgeProvider& provider) {
        return automation::status(settings, provider);
    };
    return current;
}

ActionsPage::ActionsPage(AppContext& context, SettingsPageHost& host, SettingsPage& container)
    : QWidget(&container), context_(context), host_(host), io_(context.io()) {
    relay_ = context.find<ActionRelay>();
    if (relay_ == nullptr) {
        ownRelay_ = std::make_unique<ActionRelay>(io_, sessionRelayWork(context.credentials()));
        relay_ = ownRelay_.get();
    }
    provider_ = judgeProvider(context);
    SettingsStore& store = context.settings();

    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(8);

    const TryAction tryFn = [this](const automation::Action& action, const QString& sample,
                                   std::function<void(automation::ActionResult)> done) {
        tryAction(action, sample, std::move(done));
    };
    // Closing Settings stops the Try it runs still going: nothing is left to show them.
    host_.onClosed(this, [this] { cancelTryRuns(); });

    card_ = new AgentHotkeyCard(store, *relay_, tryFn);
    connect(card_, &AgentHotkeyCard::changed, this, [this] { onCardChanged(); });
    connect(card_, &AgentHotkeyCard::captureChanged, this,
            [this](bool capturing) { host_.setHotkeyCaptureSuspended(capturing); });
    connect(card_, &AgentHotkeyCard::stylesRequested, this,
            [this] { host_.selectDestination(q(destinations::styles)); });
    layout_->addWidget(card_);
    layout_->addSpacing(10);

    panel_ = new ActionsPanel(store, *relay_, tryFn);
    connect(panel_, &ActionsPanel::actionsChanged, this, [this] { onActionsChanged(); });
    // Back from another page, say Fast judgments or Styles: read it all again.
    connect(panel_, &ActionsPanel::shown, this, [this] { refresh(); });
    connect(panel_, &ActionsPanel::captureChanged, this,
            [this](bool capturing) { host_.setHotkeyCaptureSuspended(capturing); });
    connect(panel_, &ActionsPanel::destinationRequested, this, [this](const QString& key) {
        const std::map<QString, std::string_view> destinations{
            {QStringLiteral("styles"), destinations::styles},
            {QStringLiteral("mcp"), destinations::mcp}};
        const auto found = destinations.find(key);
        host_.selectDestination(found != destinations.end() ? q(found->second) : key);
    });
    connect(card_, &AgentHotkeyCard::editRequested, this,
            [this](const automation::Action& action) { panel_->edit(q(action.id)); });
    layout_->addWidget(panel_);
    layout_->addSpacing(12);

    // No group caption: the first tile is called Route by voice itself.
    const actions_page::SearchField& routing = field("actions_routing_tile");
    routingTile_ = new SettingTile(q(routing.title), q(routing.description),
                                   ui::designIcon(QStringLiteral("microphone-blue.svg")));
    connect(routingTile_->checkbox(), &QAbstractButton::toggled, this,
            [this](bool on) { onRoutingToggled(keys::voice_routing_enabled.key(), on); });
    const actions_page::SearchField& dictation = field("actions_routing_dictation_tile");
    routingDictationTile_ = new SettingTile(q(dictation.title), q(dictation.description),
                                            ui::designIcon(QStringLiteral("text-plus-green.svg")));
    connect(routingDictationTile_->checkbox(), &QAbstractButton::toggled, this,
            [this](bool on) { onRoutingToggled(keys::voice_routing_on_dictation.key(), on); });
    container.addTileGroup(layout_, QString(), {routingTile_, routingDictationTile_});
    // Route by voice's own words for where it stands: off, what it still needs, or ready
    // (automation/routing.h).
    statusLine_ = new StatusLine(QStringLiteral("Fast judgments"),
                                 QStringLiteral("Open Settings → Meeting Mode → Fast judgments"));
    connect(statusLine_, &StatusLine::linkActivated, this,
            [this] { host_.selectDestination(q(destinations::meeting_fast)); });
    layout_->addWidget(statusLine_);

    const actions_page::SearchField& shortcut = field("actions_route_shortcut_tile");
    routeShortcutTile_ = new InfoTile(q(shortcut.title), q(shortcut.description),
                                      ui::designIcon(QStringLiteral("keyboard-green.svg")));
    routeShortcut_ = new RouteShortcutField(host_);
    // Without a shortcut (or Also listen…) nothing starts Route by voice.
    connect(routeShortcut_, &RouteShortcutField::changed, this,
            [this] { showRouting(host_.settings()); });
    routeShortcutTile_->addBody(routeShortcut_);
    container.addTileGroup(layout_, QString(), {routeShortcutTile_}, 1);
    privacy_ = settingsCaption(q(actions_page::PRIVACY_SHORTCUT));
    layout_->addWidget(privacy_);
    layout_->addStretch();

    controls_ = {
        {"actions_agent_card", card_},
        {"actions_panel", panel_},
        {"actions_routing_tile", routingTile_},
        {"actions_routing_dictation_tile", routingDictationTile_},
        {"actions_route_shortcut_tile", routeShortcutTile_},
        {"actions_routing_status", statusLine_},
        {"actions_routing_privacy", privacy_},
        {"actions_route_shortcut", routeShortcut_},
    };
}

ActionsPage::~ActionsPage() = default;

// ---- try it ----

void ActionsPage::tryAction(const automation::Action& action, const QString& sample,
                            std::function<void(automation::ActionResult)> done) {
    automation::ActionTester* tester = context_.find<automation::ActionTester>();
    if (tester == nullptr) {
        done({false,
              "Trying an action works while " + std::string(identity().product) + " is running.",
              "", false});
        return;
    }
    const std::function<void(automation::ActionResult)> callback = done;
    try {
        tester->test(action, qt::toUtf8(sample), callback);
    } catch (const std::exception& failure) {
        // The runtime failed before it could answer.
        kLog.warning("Trying an action failed before the runtime answered ({})",
                     type_display_name(typeid(failure)));
        done({false, std::string("Couldn't try it: ") + failure.what(), "", false});
    }
}

void ActionsPage::cancelTryRuns() {
    // Outside the running app nothing was started.
    if (automation::ActionTester* tester = context_.find<automation::ActionTester>()) {
        tester->cancel_tests();
    }
}

// ---- what shows ----

void ActionsPage::showAll(const Json& settings) {
    card_->load(settings);
    panel_->refresh(settings);
    showRouting(settings);
}

void ActionsPage::refresh() {
    showAll(host_.settings());
}

void ActionsPage::loadSettings(const Json& settings) {
    showAll(settings);
}

void ActionsPage::refreshPage() {
    refresh();
}

void ActionsPage::cancelCapture() {
    card_->cancelCapture();
    panel_->cancelCapture();
    routeShortcut_->cancelCapture();
}

bool ActionsPage::saveDrafts(bool ask) {
    return panel_->saveDraft(ask);
}

QWidget* ActionsPage::namedControl(std::string_view name) {
    const auto found = controls_.find(name);
    return found == controls_.end() ? nullptr : found->second.data();
}

void ActionsPage::onActionsChanged() {
    host_.notifyChanged(QStringLiteral("actions"));
    const Json settings = host_.settings();
    card_->load(settings);
    showRouting(settings);
}

void ActionsPage::onCardChanged() {
    host_.notifyChanged(QStringLiteral("actions"));
    const Json settings = host_.settings();
    // The full editor may be open on the same action.
    panel_->refresh(settings);
    showRouting(settings);
}

void ActionsPage::onRoutingToggled(std::string_view key, bool on) {
    if (host_.isLoading()) {
        return;
    }
    SettingTile* tile =
        key == keys::voice_routing_enabled.key() ? routingTile_ : routingDictationTile_;
    if (!host_.persist(key, on)) {
        const QSignalBlocker blocker(tile->checkbox());
        tile->checkbox()->setChecked(!on);
    }
    showRouting(host_.settings());
}

void ActionsPage::showRouting(const Json& settings) {
    const bool enabled = keys::voice_routing_enabled.read(settings);
    const bool onDictation = keys::voice_routing_on_dictation.read(settings);
    for (const auto& [tile, value] :
         {std::pair{routingTile_, enabled}, std::pair{routingDictationTile_, onDictation}}) {
        {
            const QSignalBlocker blocker(tile->checkbox());
            tile->checkbox()->setChecked(value);
        }
        ui::setStyleProperty(tile, ui::prop::checked, value);
    }
    routingDictationTile_->setEnabled(enabled);
    privacy_->setText(onDictation ? q(actions_page::PRIVACY_SHORTCUT_AND_DICTATION)
                                  : q(actions_page::PRIVACY_SHORTCUT));
    requestStatus(settings);
    routeShortcut_->sync();
}

void ActionsPage::requestStatus(const Json& settings) {
    // What needs no credential store shows at once, as Python computes the status on the spot;
    // the answer from `io_` follows and stands.
    if (const auto early = automation::status_without_key(settings)) {
        applyStatus(*early);
    }
    const std::uint64_t token = statusGate_.next();
    io_.post([status = routingStatus(), provider = provider_, settings, gate = statusGate_, token,
              page = QPointer<ActionsPage>(this)]() mutable {
        automation::RoutingStatus answer{false, false, "Route by voice isn't available right now.",
                                         ""};
        try {
            if (status) {
                answer = status(settings, *provider);
            }
        } catch (const std::exception& failure) {
            kLog.warning("Route by voice status unavailable: {}",
                         type_display_name(typeid(failure)));
        }
        gate.deliver(qApp, token, [page, answer] {
            if (page != nullptr) {
                page->applyStatus(answer);
            }
        });
    });
}

void ActionsPage::applyStatus(const automation::RoutingStatus& status) {
    const char* tone = status.ready     ? StatusLine::kReady
                       : status.enabled ? StatusLine::kAttention
                                        : StatusLine::kOff;
    // The link leads only where the fix is: Fast judgments live on another page; the rest
    // (actions, a way to start it) is on this one.
    statusLine_->showState(QString::fromLatin1(tone), q(status.message),
                           status.fix == automation::FIX_FAST_JUDGMENTS);
    // Ready, routing's own message carries the privacy sentence.
    privacy_->setVisible(!status.ready);
}

void registerActionsPage(AppContext& context) {
    SettingsPageSpec spec;
    spec.destination = destinations::actions;
    spec.title = q(actions_page::TITLE);
    spec.subtitle = q(actions_page::SUBTITLE);
    spec.railValue = [](const Json& settings) {
        return q(actions_page::rail_value(settings));
    };
    for (const actions_page::SearchField& entry : actions_page::search_fields()) {
        spec.searchFields.push_back(
            {std::string(entry.target), q(entry.title), q(entry.description)});
    }
    spec.controls = actions_page::control_names();
    spec.create = [&context](QWidget* parent) -> QWidget* {
        auto* container = qobject_cast<SettingsPage*>(parent);
        SettingsDialog* dialog = container != nullptr ? container->dialog() : nullptr;
        if (dialog == nullptr) {
            throw std::logic_error("The Actions page is built inside the Settings window.");
        }
        auto host = std::make_unique<DialogPageHost>(*dialog);
        auto* page = new ActionsPage(context, *host, *container);
        // Owned by the window, which deletes its pages first: the host outlives every control
        // torn down with the page (a capture field resuming hotkeys, say).
        host.release()->setParent(dialog);
        return page;
    };
    context.settingsPages().addPage(std::move(spec));
}

} // namespace fv
