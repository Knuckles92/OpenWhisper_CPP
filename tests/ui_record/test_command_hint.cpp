// Quick Record's Command Mode hint: the shortcut when there is one and a model to run it, else a
// link to Commands (tests/test_personalize_s3_quick_record_hint.py and the Quick Record half of
// tests/test_personalize_fix3_h1_command_setup.py), and the answer's path from the credential
// store to the tab.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QMetaObject>
#include <QPushButton>
#include <QSignalSpy>

#include "agents/installed_agents.h"
#include "agents/testing/fake_installed_agents.h"
#include "appkit/destinations.h"
#include "base/testing/manual_executor.h"
#include "hotkeys/grammar.h"
#include "record_fixture.h"
#include "ui_record/cleanup_profile_card.h"
#include "ui_record/command_readiness.h"
#include "ui_widgets/agent_lookup.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::Json;

// The provider's readiness the test sets (text_rewrite.provider_readiness, patched).
struct Provider {
    std::optional<bool> ready = true;
    fv::QuickRecordTab::CommandModeReadiness supplier() {
        return [this](const Json&) {
            return ready;
        };
    }
};

QString commandHintText(const std::string& key) {
    return QStringLiteral(u"Command Mode · %1 · Select text, then say how to change it.")
        .arg(QString::fromStdString(
            fv::hotkeys::format_hotkey_display(key, fv::hotkeys::platform_vocabulary())));
}

Json commandKey(const std::string& key) {
    return Json{{"command_mode", key}};
}

// tests/test_personalize_fix3_h1_command_setup.py::_tab.
Json settingsFor(const std::string& key, bool cleanup) {
    return Json{{"transcript_cleanup_enabled", cleanup}, {"hotkeys", commandKey(key)}};
}

} // namespace

// from tests/test_personalize_s3_quick_record_hint.py::test_without_a_shortcut_it_links_to_commands
TEST_CASE("without a shortcut it links to commands", "[test_personalize_s3_quick_record_hint]") {
    Provider provider;
    fvtest::ShownTab tab(Json{{"hotkeys", commandKey("")}});
    tab->setCommandModeReadiness(provider.supplier());
    QSignalSpy requested(tab.tab.get(), &fv::QuickRecordTab::settingsRequested);

    CHECK(tab->commandLink()->isVisible());
    CHECK_FALSE(tab->commandHint()->isVisible());
    tab->commandLink()->click();
    REQUIRE(requested.count() == 1);
    CHECK(requested.at(0).at(0).toString() == QStringLiteral("commands"));
}

// from tests/test_personalize_s3_quick_record_hint.py::
//      test_with_a_shortcut_it_shows_it_and_follows_changes
TEST_CASE("with a shortcut it shows it and follows changes",
          "[test_personalize_s3_quick_record_hint]") {
    Provider provider;
    fvtest::ShownTab tab(Json{{"hotkeys", commandKey("ctrl+alt+k")}});
    tab->setCommandModeReadiness(provider.supplier());

    CHECK(tab->commandHint()->text() == commandHintText("ctrl+alt+k"));
    CHECK(tab->commandHint()->isVisible());
    CHECK_FALSE(tab->commandLink()->isVisible());

    tab.settings.save_setting("hotkeys", commandKey(""));
    tab->updateHotkeys(QStringLiteral("*"), QStringLiteral("-"));
    CHECK(tab->commandLink()->isVisible());
}

// from tests/test_personalize_s3_quick_record_hint.py::test_the_link_opens_settings_on_commands
// The link asks for the destination `commands`; the controller that opens Settings on it is
// M2.28's, so the request is what is proven here, with the name Settings registers its page by.
TEST_CASE("the link opens settings on commands", "[test_personalize_s3_quick_record_hint]") {
    Provider provider;
    fvtest::ShownTab tab(Json{{"hotkeys", commandKey("")}});
    tab->setCommandModeReadiness(provider.supplier());
    std::vector<QString> asked;
    QObject::connect(tab.tab.get(), &fv::QuickRecordTab::settingsRequested,
                     [&asked](const QString& destination) { asked.push_back(destination); });

    tab->commandLink()->click();
    CHECK(asked == std::vector<QString>{QString::fromUtf8(fv::destinations::commands.data(),
                                                          fv::destinations::commands.size())});
}

// from tests/test_personalize_fix3_h1_command_setup.py::
//      test_quick_record_shows_the_hint_with_cleanup_off
TEST_CASE("quick record shows the hint with cleanup off",
          "[test_personalize_fix3_h1_command_setup]") {
    Provider provider;
    fvtest::ShownTab tab(settingsFor("", false));
    tab->setCommandModeReadiness(provider.supplier());
    CHECK_FALSE(tab->profileCard()->isVisible());
    CHECK(tab->commandLink()->isVisible());
    CHECK(tab->commandLink()->text() == QStringLiteral(u"Set a Command Mode shortcut  →"));

    tab.settings.save_setting("hotkeys", commandKey("ctrl+alt+k"));
    tab->updateHotkeys(QStringLiteral("*"), QStringLiteral("-"));
    CHECK(tab->commandHint()->isVisible());
    CHECK_FALSE(tab->commandLink()->isVisible());
    CHECK(tab->commandHint()->text() == commandHintText("ctrl+alt+k"));
}

// from tests/test_personalize_fix3_h1_command_setup.py::
//      test_quick_record_asks_for_ai_setup_until_a_model_is_ready
TEST_CASE("quick record asks for AI setup until a model is ready",
          "[test_personalize_fix3_h1_command_setup]") {
    Provider provider;
    provider.ready = false;
    fvtest::ShownTab tab(settingsFor("ctrl+alt+k", true));
    tab->setCommandModeReadiness(provider.supplier());
    QSignalSpy requested(tab.tab.get(), &fv::QuickRecordTab::settingsRequested);
    CHECK_FALSE(tab->commandHint()->isVisible());
    CHECK(tab->commandLink()->isVisible());
    CHECK(tab->commandLink()->text() == QStringLiteral(u"Set up Command Mode  →"));
    CHECK(tab->commandLink()->toolTip().contains(QStringLiteral("AI cleanup model")));
    tab->commandLink()->click();
    REQUIRE(requested.count() == 1);
    CHECK(requested.at(0).at(0).toString() == QStringLiteral("commands"));

    // Set up in Settings, then back to the main window.
    provider.ready = true;
    QEvent activate(QEvent::WindowActivate);
    QApplication::sendEvent(tab.tab.get(), &activate);
    CHECK(tab->commandHint()->isVisible());
    CHECK_FALSE(tab->commandLink()->isVisible());
}

// from tests/test_personalize_fix3_h1_command_setup.py::
//      test_quick_record_reads_again_when_an_agent_lookup_lands
// The lookup relay is M5's (installed agents); its signal is what the test sends the slot.
TEST_CASE("quick record reads again when an agent lookup lands",
          "[test_personalize_fix3_h1_command_setup]") {
    Provider provider;
    provider.ready = false;
    fvtest::ShownTab tab(settingsFor("ctrl+alt+k", false));
    tab->setCommandModeReadiness(provider.supplier());
    CHECK_FALSE(tab->commandHint()->isVisible());

    provider.ready = true;
    REQUIRE(QMetaObject::invokeMethod(tab.tab.get(), "onAgentLookedUp", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("claude_code"))));
    CHECK(tab->commandHint()->isVisible());
}

// The same through the session relay (quick_record_tab.py:190 connects lookup_relay()): a scan
// of the set installed agents notifies from its own thread, and the tab reads again.
TEST_CASE("quick record hears the session agent lookup relay", "[widget]") {
    auto agents = std::make_shared<fv::agents::FakeInstalledAgents>();
    fv::agents::set_installed_agents(agents);
    {
        Provider provider;
        provider.ready = false;
        fvtest::ShownTab tab(settingsFor("ctrl+alt+k", false));
        tab->setCommandModeReadiness(provider.supplier());
        CHECK_FALSE(tab->commandHint()->isVisible());

        provider.ready = true;
        std::thread scanner([&agents] { agents->scan_agents(true); });
        scanner.join();
        QElapsedTimer clock;
        clock.start();
        while (!tab->commandHint()->isVisible() && clock.elapsed() < 3000) {
            QCoreApplication::processEvents();
        }
        CHECK(tab->commandHint()->isVisible());
    }
    fv::agents::set_installed_agents(nullptr);
    CHECK(fv::agentLookupRelay() == nullptr);
}

// The supplier of the record feature: it answers at once from what it knows, and the credential
// store is asked on the worker.
TEST_CASE("the readiness supplier answers from the last check and asks on the worker", "[widget]") {
    fv::ManualExecutor worker;
    fv::ManualExecutor ui;
    int probes = 0;
    std::optional<bool> truth;
    fv::CommandHintReadiness readiness(
        [&](const Json&) {
            ++probes;
            return truth;
        },
        worker, ui);
    int changes = 0;
    readiness.setChanged([&changes] { ++changes; });
    const auto ask = readiness.supplier();

    SECTION("unknown until the first check lands, and nothing runs on the asking thread") {
        truth = false;
        CHECK_FALSE(ask(Json::object()).has_value());
        CHECK(probes == 0);
        worker.run_all();
        CHECK(probes == 1);
        // The answer is delivered on the GUI side.
        CHECK(changes == 0);
        ui.run_all();
        CHECK(changes == 1);
        CHECK(ask(Json::object()) == std::optional<bool>(false));
    }
    SECTION("an answer that did not change tells nobody") {
        truth = true;
        ask(Json::object());
        worker.run_all();
        ui.run_all();
        CHECK(changes == 1);
        ask(Json::object());
        worker.run_all();
        ui.run_all();
        CHECK(probes == 2);
        CHECK(changes == 1);
    }
    SECTION("a question asked while a check runs is checked when it ends, once") {
        truth = true;
        ask(Json::object());
        ask(Json{{"transcript_cleanup_model", "a"}});
        ask(Json{{"transcript_cleanup_model", "b"}});
        CHECK(worker.run_all() == 1);
        ui.run_all();
        CHECK(worker.run_all() == 1);
        ui.run_all();
        CHECK(probes == 2);
    }
    SECTION("the refresh a new answer causes asks again only for settings not checked") {
        const Json settings{{"transcript_cleanup_model", "a"}};
        Json refreshWith = settings;
        std::vector<std::optional<bool>> seen;
        readiness.setChanged([&] { seen.push_back(ask(refreshWith)); });
        truth = true;
        ask(settings);
        CHECK(worker.run_all() == 1);
        ui.run_all();
        // The tab read the answer that landed, for the settings it was found for: no new check.
        CHECK(seen == std::vector<std::optional<bool>>{true});
        CHECK(worker.run_all() == 0);

        // Settings changed with no refresh in between: the refresh the answer causes checks them.
        truth = false;
        ask(settings);
        refreshWith = Json{{"transcript_cleanup_model", "b"}};
        CHECK(worker.run_all() == 1);
        ui.run_all();
        CHECK(seen.back() == std::optional<bool>(false));
        CHECK(worker.run_all() == 1);
        CHECK(probes == 3);
    }
    SECTION("a check that throws keeps the last answer") {
        bool fail = false;
        fv::CommandHintReadiness flaky(
            [&](const Json&) -> std::optional<bool> {
                if (fail) {
                    throw std::runtime_error("keyring");
                }
                return false;
            },
            worker, ui);
        const auto askFlaky = flaky.supplier();
        askFlaky(Json::object());
        worker.run_all();
        ui.run_all();
        fail = true;
        CHECK(askFlaky(Json::object()) == std::optional<bool>(false));
        worker.run_all();
        ui.run_all();
        // The failed check is over: the next question starts another.
        CHECK(askFlaky(Json::object()) == std::optional<bool>(false));
        CHECK(worker.run_all() == 1);
    }
    SECTION("an object that is gone leaves the supplier answering unknown") {
        auto gone = std::make_unique<fv::CommandHintReadiness>(
            [&](const Json&) { return std::optional<bool>(true); }, worker, ui);
        const auto askGone = gone->supplier();
        askGone(Json::object());
        gone.reset();
        worker.run_all();
        ui.run_all();
        CHECK_FALSE(askGone(Json::object()).has_value());
    }
}

TEST_CASE("a readiness that lands tells the tab to read the hint again", "[widget]") {
    fv::ManualExecutor worker;
    fv::ManualExecutor ui;
    fvtest::ShownTab tab(settingsFor("ctrl+alt+k", false));
    std::optional<bool> truth = false;
    fv::CommandHintReadiness readiness([&](const Json&) { return truth; }, worker, ui);
    readiness.setChanged([&tab] { tab->refreshCommandHint(); });
    tab->setCommandModeReadiness(readiness.supplier());
    // Until the check lands the provider is assumed to work.
    CHECK(tab->commandHint()->isVisible());

    worker.run_all();
    ui.run_all();
    CHECK_FALSE(tab->commandHint()->isVisible());
    CHECK(tab->commandLink()->text() == QStringLiteral(u"Set up Command Mode  →"));

    truth = true;
    QEvent activate(QEvent::WindowActivate);
    QApplication::sendEvent(tab.tab.get(), &activate);
    worker.run_all();
    ui.run_all();
    CHECK(tab->commandHint()->isVisible());
}
