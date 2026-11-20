#pragma once

// Settings > Automate > Actions inside a real Settings window over fakes (OpenWhisper's
// tests/test_automation_ui_actions_page.py make_dialog and its fixtures):
// - an in-memory settings file, the GUI executor and an io pool, a data folder of its own, the
//   Automation feature provided and installed as the app does it, and a stand-in page for every
//   other destination;
// - `questions`: every confirmation asked, as (title, text), answered `answer` (Yes unless a test
//   says otherwise), never a real message box;
// - the agent scan (agents.available): FakeInstalledAgents set as the session's instance, Claude
//   Code and Codex found and OpenCode missing until a test calls setFound; a scan is what makes
//   a change visible, as the Look again link's does;
// - signing secrets (runtime.ensure_action_secret, delete_action_secret): a map, each call noting
//   whether it ran off the GUI thread;
// - `copied`: what a signing secret copy put on the clipboard (action_editor.copy_text);
// - "Try it" (dialog.on_try_action, on_cancel_try_runs): tester() provides a FakeTester to the
//   context; without it the page has no ActionTester, as outside the running app;
// - the Hotkeys page's save path for one standard shortcut, as set_standard_hotkey does it;
// - `captures`: the window's capture pauses (on_profile_hotkey_capture); `changes`: what the
//   window said changed (on_settings_changed);
// - Route by voice's status: automation::status over a FakeJudgeProvider with a key, unless a
//   test fixes it with setStatus.

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QMessageBox>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <catch2/catch_test_macros.hpp>

#include "agents/agent_actions.h"
#include "agents/installed_agents.h"
#include "agents/testing/fake_installed_agents.h"
#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "automation/action_model.h"
#include "automation/action_result.h"
#include "automation/routing.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/json.h"
#include "base/paths.h"
#include "hotkeys/grammar.h"
#include "hotkeys/testing/fake_capture.h"
#include "llm/testing/fake_judge.h"
#include "settings/hotkeys.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "text/cleanup_profiles.h"
#include "ui_automation/action_agent_card.h"
#include "ui_automation/action_bindings.h"
#include "ui_automation/action_editor.h"
#include "ui_automation/action_fields.h"
#include "ui_automation/action_gallery.h"
#include "ui_automation/action_panel.h"
#include "ui_automation/action_prompts.h"
#include "ui_automation/action_relay.h"
#include "ui_automation/actions_page.h"
#include "ui_automation/automation_feature.h"
#include "ui_hotkeys/hotkeys_page.h"
#include "ui_hotkeys/personal_binding_sources.h"
#include "ui_hotkeys/standard_hotkeys.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_settings/settings_window.h"
#include "ui_widgets/setting_tile.h"

namespace fvtest {

namespace dest = fv::destinations;
using fv::Json;

inline QString qs(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

inline QString display(std::string_view hotkey) {
    return qs(fv::hotkeys::format_hotkey_display(hotkey, fv::hotkeys::platform_vocabulary()));
}

// pump(times): turns the event loop, deferred deletes included.
inline void pump(int times = 6) {
    for (int i = 0; i < times; ++i) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

// wait_for: turns the event loop until `condition` holds or 5 s pass; the last answer.
inline bool waitFor(const std::function<bool()>& condition, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QCoreApplication::processEvents();
    return condition();
}

// One page heading per destination the rail lists, for the stand-ins.
struct StandIn {
    std::string_view key;
    const char* title;
};

inline const std::vector<StandIn>& standIns() {
    static const std::vector<StandIn> all{
        {dest::overview, "Home"},        {dest::voice_model, "Voice model"},
        {dest::recording, "Recording"},  {dest::cleanup, "AI cleanup"},
        {dest::styles, "Apps & styles"}, {dest::commands, "Commands"},
        {dest::hooks, "Hooks"},          {dest::meeting_fast, "Fast judgments"},
        {dest::general, "General"},      {dest::hotkeys, "Hotkeys"},
        {dest::mcp, "API & MCP"},
    };
    return all;
}

// "Try it" through the app: `handler` gets each run (none: the run never answers), `cancel`
// runs on cancel_tests (counted in `cancels`).
class FakeTester final : public fv::automation::ActionTester {
public:
    std::function<void(const fv::automation::Action&, const std::string&,
                       std::function<void(fv::automation::ActionResult)>)>
        handler;
    std::function<int()> cancel;
    int cancels = 0;

    void test(const fv::automation::Action& action, std::string sample,
              std::function<void(fv::automation::ActionResult)> done) override {
        if (handler) {
            handler(action, sample, std::move(done));
        }
    }
    int cancel_tests() override {
        ++cancels;
        return cancel ? cancel() : 0;
    }
};

// What the secret calls did, guarded: they run on the io pool.
struct Secrets {
    std::mutex mutex;
    std::map<std::string, std::string> saved;
    // Whether each call ran on the GUI thread.
    std::vector<bool> onGuiThread;
    int made = 0;
    // Set: ensure throws this message.
    std::optional<std::string> refuse;

    std::optional<std::string> get(const std::string& id) {
        std::lock_guard lock(mutex);
        const auto found = saved.find(id);
        return found == saved.end() ? std::nullopt : std::optional(found->second);
    }
    void put(const std::string& id, const std::string& secret) {
        std::lock_guard lock(mutex);
        saved[id] = secret;
    }
    std::size_t calls() {
        std::lock_guard lock(mutex);
        return onGuiThread.size();
    }
    bool anyOnGuiThread() {
        std::lock_guard lock(mutex);
        for (bool gui : onGuiThread) {
            if (gui) {
                return true;
            }
        }
        return false;
    }
};

// The window as the Hotkeys page's host (HotkeysPageAdapter's part of it): its message line,
// capture pauses, destinations and persist; the standard shortcuts are saved as they are.
class RigHotkeysHost final : public QObject, public fv::HotkeysPageHost {
public:
    RigHotkeysHost(fv::SettingsDialog& dialog, fv::SettingsStore& store)
        : QObject(&dialog), dialog_(&dialog), store_(store) {}

    void showMessage(const QString& message) override { dialog_->showMessage(message); }
    void setCaptureSuspended(bool suspended) override {
        dialog_->setHotkeyCaptureSuspended(suspended);
    }
    void hotkeysChanged(const Json& hotkeys) override { fv::save_hotkey_settings(store_, hotkeys); }
    void triggerModeChanged(const QString&) override {}
    void openDestination(const QString& destination) override {
        dialog_->selectDestination(destination);
    }
    bool persist(std::string_view key, const Json& value) override {
        return dialog_->persist(key, value);
    }

private:
    fv::SettingsDialog* dialog_;
    fv::SettingsStore& store_;
};

class ActionsRig {
public:
    // `realHotkeys`: the Hotkeys page is the real one, with the profile and transform rows the
    // app registers on it (ui_controller's PersonalizeWiring) and fake global captures.
    explicit ActionsRig(const Json& saved = Json::object(), bool realHotkeys = false)
        : paths_(fv::Paths::resolve(fv::identity(), fv::system_env())), settings_(saved), ui_(qApp),
          context_(services()), io_("actions-test-io", 2) {
        guiThread_ = std::this_thread::get_id();
        previousQuestion_ = fv::action_prompts::question();
        previousCopy_ = fv::action_prompts::copyText();
        previousStatus_ = fv::ActionsPage::routingStatus();
        fv::action_prompts::question() = [this](QWidget*, const QString& title, const QString& text,
                                                QMessageBox::StandardButtons,
                                                QMessageBox::StandardButton) {
            questions.emplace_back(title, text);
            return answer;
        };
        fv::action_prompts::copyText() = [this](const QString& text) {
            copied << text;
        };
        fv::ActionsPage::routingStatus() = [this](const Json& settings,
                                                  fv::llm::JudgeProvider& provider) {
            {
                std::lock_guard lock(statusMutex_);
                if (fixedStatus_) {
                    return *fixedStatus_;
                }
                if (statusFn_) {
                    return statusFn_(settings);
                }
            }
            return fv::automation::status(settings, provider);
        };

        agents_ = std::make_shared<fv::agents::FakeInstalledAgents>();
        fv::agents::set_installed_agents(agents_);
        setFound({{"claude_code", true}, {"codex", true}, {"opencode_cli", false}});

        context_.provide<fv::ActionRelay>(std::make_shared<fv::ActionRelay>(io_, relayWork()));
        judges_ = std::make_shared<fv::llm::FakeJudgeProvider>();
        context_.provide<fv::llm::JudgeProvider>(judges_);
        if (realHotkeys) {
            fv::SettingsPageSpec spec;
            spec.destination = dest::hotkeys;
            spec.title = QStringLiteral("Hotkeys");
            spec.create = [this](QWidget* parent) -> QWidget* {
                auto* container = qobject_cast<fv::SettingsPage*>(parent);
                fv::SettingsDialog* dialog = container->dialog();
                auto* host = new RigHotkeysHost(*dialog, settings_);
                auto* page = new fv::HotkeysPage(settings_, *host, container);
                page->setCaptureFactory(captureFactory_.factory());
                fv::registerPersonalBindingSources(*page);
                return page;
            };
            context_.settingsPages().addPage(std::move(spec));
        }
        context_.provide<fv::SettingsWindow>(std::make_shared<fv::SettingsWindow>(context_));
        feature_.provide(context_);
        feature_.install(context_);
        for (const StandIn& standIn : standIns()) {
            if (context_.settingsPages().page(standIn.key) == nullptr) {
                fv::SettingsPageSpec spec;
                spec.destination = standIn.key;
                spec.title = QString::fromUtf8(standIn.title);
                spec.create = [](QWidget* parent) {
                    auto* page = qobject_cast<fv::SettingsPage*>(parent);
                    page->addTileGroup(page->pageLayout(), QStringLiteral("Stand-in group"),
                                       {new fv::InfoTile(QStringLiteral("Stand-in tile"),
                                                         QStringLiteral("A stand-in page."))});
                    return page;
                };
                context_.settingsPages().addPage(std::move(spec));
            }
        }
    }

    ~ActionsRig() {
        answer = QMessageBox::Discard;
        for (auto& dialog : dialogs_) {
            dialog->close();
        }
        dialogs_.clear();
        // The window SettingsWindow made, when a test asked it for one.
        delete context_.service<fv::SettingsWindow>().dialog();
        pump(3);
        io_.shutdown(std::chrono::seconds(5));
        fv::agents::set_installed_agents(nullptr);
        fv::action_prompts::question() = previousQuestion_;
        fv::action_prompts::copyText() = previousCopy_;
        fv::ActionsPage::routingStatus() = previousStatus_;
    }
    ActionsRig(const ActionsRig&) = delete;
    ActionsRig& operator=(const ActionsRig&) = delete;

    // make_dialog: the window, on the Actions page unless `select` is false, at `size` when
    // given, shown.
    fv::SettingsDialog& dialog(bool select = true, std::optional<QSize> size = std::nullopt) {
        auto made = std::make_unique<fv::SettingsDialog>(context_);
        made->availableScreenRect = [] {
            return QRect(0, 0, 1920, 1080);
        };
        fv::SettingsDialog& dialog = *made;
        dialog.setStandardHotkeyHandler(
            [this, &dialog](const QString& action, const QString& hotkey) {
                return setStandardHotkey(dialog, fv::qt::toUtf8(action), fv::qt::toUtf8(hotkey));
            });
        QObject::connect(&dialog, &fv::SettingsDialog::settingsChanged,
                         [this](const QString& kind) { changes << kind; });
        QObject::connect(&dialog, &fv::SettingsDialog::hotkeyCaptureSuspended,
                         [this](bool suspended) { captures.push_back(suspended); });
        // What the feature does for the app's one window when SettingsWindow creates it.
        new fv::ActionBindingsWiring(dialog);
        dialogs_.push_back(std::move(made));
        if (select) {
            dialog.selectDestination(qs(dest::actions));
        }
        if (size) {
            dialog.resize(*size);
        }
        dialog.show();
        pump();
        return dialog;
    }

    fv::SettingsDialog& current() { return *dialogs_.back(); }
    fv::ActionsPage& page() {
        auto* content = dynamic_cast<fv::ActionsPage*>(current().pageContent(dest::actions));
        REQUIRE(content != nullptr);
        return *content;
    }
    fv::ActionsPanel& panel() { return *page().panel(); }
    fv::ActionEditor& editor() { return *page().panel()->editor(); }
    fv::AgentHotkeyCard& card() { return *page().agentCard(); }

    // The gallery's starting point `id`.
    fv::automation::Preset preset(std::string_view id) {
        for (const fv::automation::Preset& entry : fv::automation::PRESETS()) {
            if (entry.id == id) {
                return entry;
            }
        }
        if (id == "own-link") {
            return fv::action_gallery::ownLink();
        }
        FAIL("no preset " << id);
        return {};
    }

    std::vector<fv::automation::Action> actions() {
        return fv::automation::load_actions(*settings_.snapshot());
    }
    std::optional<fv::automation::Action> stored(std::string_view id) {
        return fv::automation::find_action(*settings_.snapshot(), id);
    }
    Json saved(std::string_view key) {
        const auto snapshot = settings_.snapshot();
        const auto found = snapshot->find(key);
        return found == snapshot->end() ? Json() : *found;
    }

    // agents.found: which agents the next scan finds. A scan runs now, off the GUI thread, so
    // agents::available() says so at once (as the Python fixture's patched function does).
    void setFound(const std::map<std::string, bool>& found) {
        for (const std::string_view id : fv::automation::AGENTS) {
            const auto it = found.find(std::string(id));
            if (it != found.end() && it->second) {
                fv::agents::InstalledAgent agent;
                agent.id = std::string(id);
                agent.path = std::filesystem::path("C:/fake") / std::string(id);
                agent.version = "1.0.0";
                agents_->set_agent(id, agent);
            } else {
                agents_->set_agent(id, std::nullopt);
            }
        }
        std::thread scan([this] { agents_->scan_agents(true); });
        scan.join();
    }
    // Which agents the next scan finds, without scanning now.
    void setFoundOnNextScan(const std::map<std::string, bool>& found) {
        for (const std::string_view id : fv::automation::AGENTS) {
            const auto it = found.find(std::string(id));
            if (it != found.end() && it->second) {
                fv::agents::InstalledAgent agent;
                agent.id = std::string(id);
                agent.path = std::filesystem::path("C:/fake") / std::string(id);
                agent.version = "1.0.0";
                agents_->set_agent(id, agent);
            } else {
                agents_->set_agent(id, std::nullopt);
            }
        }
    }
    // A rig whose scan has never run: agents::available() is {} until the page asks for one.
    void forgetScan() {
        agents_ = std::make_shared<fv::agents::FakeInstalledAgents>();
        fv::agents::set_installed_agents(agents_);
    }

    // dialog.on_try_action: the context's ActionTester, provided on first use.
    FakeTester& tester() {
        if (tester_ == nullptr) {
            tester_ = std::make_shared<FakeTester>();
            context_.provide<fv::automation::ActionTester>(tester_);
        }
        return *tester_;
    }

    // routing.status monkeypatched to a fixed answer (nullopt: the real one again).
    void setStatus(std::optional<fv::automation::RoutingStatus> status) {
        std::lock_guard lock(statusMutex_);
        fixedStatus_ = std::move(status);
    }
    // routing.status monkeypatched to a function of the settings.
    void setStatusFunction(std::function<fv::automation::RoutingStatus(const Json&)> status) {
        std::lock_guard lock(statusMutex_);
        statusFn_ = std::move(status);
    }
    // routing.status(settings) as the page would get it now.
    std::string routingWords() {
        return fv::automation::status(*settings_.snapshot(), *judges_).message;
    }

    // set_standard_hotkey as the window's handler runs it (for a test that wraps the handler).
    QString standardHotkey(fv::SettingsDialog& made, const QString& action, const QString& hotkey) {
        return setStandardHotkey(made, fv::qt::toUtf8(action), fv::qt::toUtf8(hotkey));
    }

    fv::MemorySettingsStore& settings() { return settings_; }
    fv::AppContext& context() { return context_; }
    fv::llm::FakeJudgeProvider& judges() { return *judges_; }
    fv::agents::FakeInstalledAgents& agents() { return *agents_; }

    std::vector<std::pair<QString, QString>> questions;
    QMessageBox::StandardButton answer = QMessageBox::Yes;
    QStringList copied;
    QStringList changes;
    std::vector<bool> captures;
    Secrets secrets;

private:
    fv::AppServices services() {
        fv::AppServices made;
        made.identity = &fv::identity();
        made.paths = &paths_;
        made.settings = &settings_;
        made.ui = &ui_;
        made.io = &io_;
        return made;
    }

    fv::ActionRelayWork relayWork() {
        fv::ActionRelayWork work;
        work.scanAgents = [this](bool refresh) {
            std::shared_ptr<fv::agents::InstalledAgents> agents =
                fv::agents::installed_agents_if_set();
            if (agents) {
                agents->scan_agents(refresh);
            }
        };
        work.ensureSecret = [this](const std::string& id) {
            std::lock_guard lock(secrets.mutex);
            secrets.onGuiThread.push_back(std::this_thread::get_id() == guiThread_);
            if (secrets.refuse) {
                throw std::runtime_error(*secrets.refuse);
            }
            auto found = secrets.saved.find(id);
            if (found == secrets.saved.end()) {
                found = secrets.saved.emplace(id, "whsec_" + std::to_string(++secrets.made)).first;
            }
            return found->second;
        };
        work.deleteSecret = [this](const std::string& id) {
            std::lock_guard lock(secrets.mutex);
            secrets.onGuiThread.push_back(std::this_thread::get_id() == guiThread_);
            secrets.saved.erase(id);
        };
        return work;
    }

    // settings_dialog.py set_standard_hotkey.
    QString setStandardHotkey(fv::SettingsDialog& made, const std::string& action,
                              const std::string& hotkey) {
        Json current = fv::default_hotkeys();
        const Json stored = fv::load_hotkey_settings(settings_);
        if (stored.is_object()) {
            for (const auto& [key, value] : stored.items()) {
                current[key] = value;
            }
        }
        Json updated = current;
        updated[action] = hotkey;
        fv::text::ShortcutContext shortcuts;
        shortcuts.standard_hotkeys = updated;
        (*shortcuts.standard_hotkeys)[action] = "";
        shortcuts.actions = {};
        for (const auto& saved : fv::automation::load_actions(*settings_.snapshot())) {
            shortcuts.actions.push_back({saved.id, saved.name, saved.hotkey});
        }
        const std::string conflict =
            fv::text::profile_hotkey_conflict(hotkey, *settings_.snapshot(), {}, shortcuts);
        if (!conflict.empty()) {
            const QString error =
                QStringLiteral("That shortcut is already used by %1.").arg(qs(conflict));
            made.showMessage(error);
            return error;
        }
        fv::save_hotkey_settings(settings_, updated);
        made.showMessage(qs(fv::standard_hotkeys::label(action)) +
                         QStringLiteral(" hotkey updated."));
        made.refreshRailValues();
        return {};
    }

    fv::testkit::TempDataRoot root_;
    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::qt::QtUiExecutor ui_;
    fv::AppContext context_;
    // After the context: destroyed first, so no io task outlives what it reaches.
    fv::ThreadPool io_;
    fv::AutomationFeature feature_;
    fv::hotkeys::FakeCaptureFactory captureFactory_;
    std::shared_ptr<fv::agents::FakeInstalledAgents> agents_;
    std::shared_ptr<fv::llm::FakeJudgeProvider> judges_;
    std::shared_ptr<FakeTester> tester_;
    std::vector<std::unique_ptr<fv::SettingsDialog>> dialogs_;
    std::thread::id guiThread_;
    fv::action_prompts::Question previousQuestion_;
    fv::action_prompts::CopyText previousCopy_;
    fv::ActionsPage::RoutingStatusFn previousStatus_;
    std::mutex statusMutex_;
    std::optional<fv::automation::RoutingStatus> fixedStatus_;
    std::function<fv::automation::RoutingStatus(const Json&)> statusFn_;
};

} // namespace fvtest
