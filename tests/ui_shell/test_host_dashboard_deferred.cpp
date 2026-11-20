// tests/test_deferred_screens.py's Host Mode case: the host dashboard is built once, the first
// time Host Mode shows, and turning Host Mode on builds no other screen (Upload File and Meeting
// Mode stay unbuilt, with what was sent to them kept for later).
#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QPointer>
#include <QWidget>

#include "appkit/order.h"
#include "appkit/registries.h"
#include "shell_fixture.h"
#include "ui_shell/main_window.h"

// from
// tests/test_deferred_screens.py::test_host_dashboard_materializes_once_and_receives_queued_values
TEST_CASE("the host dashboard is built once and Host Mode builds no other screen", "[widget]") {
    fvtest::ShellFixture shell;
    int meetingBuilds = 0;
    shell.context().screens().define("meeting", fv::order::tab::meeting_mode,
                                     QStringLiteral("Meeting Mode"), [&](QWidget* parent) {
                                         ++meetingBuilds;
                                         return new QWidget(parent);
                                     });
    auto window = shell.window();
    window->show();
    fvtest::settle();
    // Values sent to screens nobody has opened are kept, not applied.
    int applied = 0;
    window->updateScreen("upload", "set_engine_busy", [&applied](QWidget*) { ++applied; });
    window->updateScreen("meeting", "set_dashboard_available", [&applied](QWidget*) { ++applied; });
    CHECK(applied == 0);

    int builds = 0;
    QPointer<QWidget> dashboard;
    window->setHostDashboardFactory([&](QWidget* parent) {
        ++builds;
        dashboard = new QWidget(parent);
        return dashboard.data();
    });
    auto* command = shell.context().menus().action(fv::menu_id::hostMode);
    REQUIRE(command != nullptr);
    CHECK(builds == 0);

    command->trigger();
    REQUIRE(dashboard);
    QWidget* first = dashboard.data();
    CHECK(builds == 1);
    command->trigger();
    command->trigger();
    CHECK(builds == 1);
    CHECK(dashboard.data() == first);
    CHECK(shell.context().screens().existing("upload") == nullptr);
    CHECK(shell.context().screens().existing("meeting") == nullptr);
    CHECK(meetingBuilds == 0);
    CHECK(applied == 0);
    command->trigger();
}
