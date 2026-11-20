// A saved window geometry off the screen or taller than it (tests/test_omarchy_ui.py's native
// window case, without the Omarchy tile): the window comes back on the screen it opens on.
#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QRect>

#include "appkit/registries.h"
#include "settings/keys.h"
#include "shell_fixture.h"
#include "ui_shell/main_window.h"

using fvtest::ShellFixture;

// from tests/test_omarchy_ui.py::test_native_window_ignores_saved_offscreen_geometry
TEST_CASE("host mode ignores a saved geometry off the screen", "[widget]") {
    ShellFixture shell;
    shell.settings().save_setting(
        fv::keys::host_window_geometry.key(),
        fv::Json{{"x", -2000}, {"y", -17}, {"width", 780}, {"height", 1000}});
    auto window = shell.window();
    const QRect screen(0, 0, 1280, 720);
    window->availableScreenRect = [screen] {
        return screen;
    };
    window->maxUsableHeight = [screen] {
        return screen.height();
    };
    QAction* host = shell.context().menus().action(fv::menu_id::hostMode);
    REQUIRE(host);
    window->setHostDashboardFactory([](QWidget* parent) { return new QWidget(parent); });

    host->trigger();

    REQUIRE(window->isHostMode());
    CHECK(window->height() < 1000);
    CHECK(screen.contains(window->geometry()));
}
