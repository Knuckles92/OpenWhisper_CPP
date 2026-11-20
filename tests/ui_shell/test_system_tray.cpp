// The tray icon and its menu, through TrayEnvironment: no icon reaches the desktop of the session
// running the tests (architecture amendment 14). Ported from tests/test_system_tray.py.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QSignalSpy>

#include "base/log.h"
#include "shell_fixture.h"
#include "ui_shell/main_window.h"
#include "ui_shell/system_tray.h"

using fv::SystemTray;
using fv::TrayEnvironment;
using fvtest::ShellFixture;

namespace {

// A tray the desktop has (available) whose icon is never put in the notification area.
TrayEnvironment fakeTray(bool available, int* shown = nullptr) {
    TrayEnvironment environment;
    environment.available = [available] {
        return available;
    };
    environment.showIcon = [shown](QSystemTrayIcon&) {
        if (shown != nullptr) {
            ++*shown;
        }
    };
    return environment;
}

QStringList actionTexts(QMenu* menu) {
    QStringList texts;
    for (QAction* action : menu->actions()) {
        texts << (action->isSeparator() ? QStringLiteral("---") : action->text());
    }
    return texts;
}

class CapturedLog final : public fv::log::Sink {
public:
    void write(const fv::log::Record& record) override {
        lines.push_back(std::string(record.logger) + ": " + std::string(record.message));
    }
    std::vector<std::string> lines;
};

} // namespace

// from tests/test_system_tray.py::TestOtherPlatformTrayClicks::test_context_menu_stays_attached
TEST_CASE("context menu stays attached", "[test_system_tray][widget]") {
    ShellFixture shell;
    auto window = shell.window();
    SystemTray tray(*window, fakeTray(true));
    CHECK(tray.contextMenu() == tray.menu());
}

// from
// tests/test_system_tray.py::TestOtherPlatformTrayClicks::test_double_click_restores_and_single_click_does_not
TEST_CASE("double click restores and single click does not", "[test_system_tray][widget]") {
    ShellFixture shell;
    auto window = shell.window();
    SystemTray tray(*window, fakeTray(true));
    REQUIRE_FALSE(window->isVisible());

    tray.onActivated(QSystemTrayIcon::Trigger);
    QApplication::processEvents();
    CHECK_FALSE(window->isVisible());

    tray.onActivated(QSystemTrayIcon::DoubleClick);
    QApplication::processEvents();
    CHECK(window->isVisible());
}

TEST_CASE("the tray menu holds OpenWhisper's items in order", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    SystemTray tray(*window, fakeTray(true));
    CHECK(actionTexts(tray.menu()) ==
          QStringList{
              QStringLiteral("Show"), QStringLiteral("Hide"), QStringLiteral("---"),
              QStringLiteral("Start Recording"), QStringLiteral("Copy original of last dictation"),
              QStringLiteral("Start Walkthrough"), QStringLiteral("Take Screenshot"),
              QStringLiteral("Language"), QStringLiteral("---"), QStringLiteral("Start Meeting"),
              QStringLiteral("Open Meeting Dashboard"), QStringLiteral("---"),
              QStringLiteral("Scratchpad"), QStringLiteral("Stats"), QStringLiteral("Settings"),
              QStringLiteral("---"), QString(), QStringLiteral("Exit")});
    REQUIRE(tray.languageMenu() != nullptr);
    fv::MenuRegistry& menus = shell.context().menus();
    CHECK_FALSE(menus.action(fv::tray_id::meetingDashboard)->isEnabled());
    CHECK_FALSE(menus.action(fv::tray_id::update)->isVisible());
    for (const char* id : {fv::tray_id::settings, fv::tray_id::update, fv::tray_id::exit}) {
        CHECK(menus.action(id)->menuRole() == QAction::NoRole);
    }
    CHECK(tray.toolTip() == QStringLiteral("FioriVoce"));
    CHECK_FALSE(tray.icon().isNull());
}

TEST_CASE("tray items reach the window and the tray's signals", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    SystemTray tray(*window, fakeTray(true));
    fv::MenuRegistry& menus = shell.context().menus();

    menus.action(fv::tray_id::show)->trigger();
    QApplication::processEvents();
    CHECK(window->isVisible());
    menus.action(fv::tray_id::hide)->trigger();
    CHECK_FALSE(window->isVisible());

    QSignalSpy settings(window.get(), &fv::MainWindow::settingsRequested);
    menus.action(fv::tray_id::settings)->trigger();
    CHECK(settings.count() == 1);

    QSignalSpy exit(&tray, &SystemTray::exitRequested);
    QSignalSpy toggle(&tray, &SystemTray::toggleRecording);
    QSignalSpy copy(&tray, &SystemTray::copyLastOriginalRequested);
    QSignalSpy walkthrough(&tray, &SystemTray::walkthroughRequested);
    QSignalSpy screenshot(&tray, &SystemTray::screenshotRequested);
    QSignalSpy meeting(&tray, &SystemTray::meetingToggleRequested);
    QSignalSpy dashboard(&tray, &SystemTray::meetingDashboardRequested);
    QSignalSpy scratchpad(&tray, &SystemTray::scratchpadRequested);
    QSignalSpy stats(&tray, &SystemTray::statsRequested);
    QSignalSpy update(&tray, &SystemTray::updateRequested);
    QSignalSpy language(&tray, &SystemTray::languageMenuRequested);
    // Open Meeting Dashboard is disabled until a meeting runs, and a disabled action does not
    // trigger.
    tray.setMeetingActive(true);
    for (const char* id :
         {fv::tray_id::exit, fv::tray_id::toggleRecording, fv::tray_id::copyOriginal,
          fv::tray_id::walkthrough, fv::tray_id::screenshot, fv::tray_id::meeting,
          fv::tray_id::meetingDashboard, fv::tray_id::scratchpad, fv::tray_id::stats,
          fv::tray_id::update}) {
        menus.action(id)->trigger();
    }
    for (const QSignalSpy* spy : {&exit, &toggle, &copy, &walkthrough, &screenshot, &meeting,
                                  &dashboard, &scratchpad, &stats, &update}) {
        CHECK(spy->count() == 1);
    }
    Q_EMIT tray.languageMenu()->aboutToShow();
    REQUIRE(language.count() == 1);
    CHECK(language.at(0).at(0).value<QMenu*>() == tray.languageMenu());
}

TEST_CASE("tray state setters change the items' text", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    SystemTray tray(*window, fakeTray(true));
    fv::MenuRegistry& menus = shell.context().menus();

    tray.setRecording(true);
    CHECK(menus.action(fv::tray_id::toggleRecording)->text() == QStringLiteral("Stop Recording"));
    tray.setRecording(false);
    CHECK(menus.action(fv::tray_id::toggleRecording)->text() == QStringLiteral("Start Recording"));

    tray.setWalkthroughActive(true);
    CHECK(menus.action(fv::tray_id::walkthrough)->text() == QStringLiteral("Finish Walkthrough"));
    tray.setWalkthroughActive(false);
    CHECK(menus.action(fv::tray_id::walkthrough)->text() == QStringLiteral("Start Walkthrough"));

    tray.setUpdateAvailable(QStringLiteral("0.2.0"));
    CHECK(menus.action(fv::tray_id::update)->text() == QStringLiteral("Update to 0.2.0…"));
    CHECK(menus.action(fv::tray_id::update)->isVisible());
    CHECK(tray.toolTip() == QStringLiteral("FioriVoce: update 0.2.0 available"));
    tray.setUpdateAvailable(QString());
    CHECK_FALSE(menus.action(fv::tray_id::update)->isVisible());
    CHECK(tray.toolTip() == QStringLiteral("FioriVoce"));

    tray.setMeetingActive(true);
    CHECK(menus.action(fv::tray_id::meeting)->text() == QStringLiteral("End Meeting"));
    CHECK(menus.action(fv::tray_id::meetingDashboard)->isEnabled());
    tray.setMeetingActive(true, false);
    CHECK_FALSE(menus.action(fv::tray_id::meetingDashboard)->isEnabled());
    tray.setMeetingActive(false);
    CHECK(menus.action(fv::tray_id::meeting)->text() == QStringLiteral("Start Meeting"));
    CHECK_FALSE(menus.action(fv::tray_id::meetingDashboard)->isEnabled());
}

TEST_CASE("a desktop without a tray never shows the icon and says so", "[widget]") {
    auto log = std::make_shared<CapturedLog>();
    fv::log::add_sink(log);
    ShellFixture shell;
    auto window = shell.window();
    int shown = 0;
    {
        SystemTray tray(*window, fakeTray(false, &shown));
        CHECK_FALSE(tray.available());
    }
    {
        SystemTray tray(*window, fakeTray(true, &shown));
        CHECK(tray.available());
    }
    fv::log::remove_sink(log);
    CHECK(shown == 1);
    const auto has = [&log](const std::string& line) {
        return std::ranges::find(log->lines, line) != log->lines.end();
    };
    CHECK(has("ui_qt.system_tray: No system tray is available; close-to-tray and tray controls "
              "are disabled"));
    CHECK(has("ui_qt.system_tray: System tray initialized"));
}
