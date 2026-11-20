// a32 MW-11 with more than one monitor (main_window.py _apply_saved_geometry): a window saved on
// a second monitor comes back there, primary first among the screens it overlaps; a monitor
// that is gone leaves the window centred on the one it opens on; a short screen (1280x800)
// clamps the saved height. The screens are MainWindow::screenAreas, a fake list here.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QRect>

#include "settings/keys.h"
#include "shell_fixture.h"
#include "ui_shell/main_window.h"
#include "uimath/window_geometry.h"

using fvtest::ShellFixture;

namespace {

fv::Json savedAt(int x, int y, int width, int height) {
    return fv::Json{{"x", x},
                    {"y", y},
                    {"width", width},
                    {"height", height},
                    {"format", std::string(fv::kCollapsedContentFormat)},
                    {"history_expanded", false}};
}

const QRect kPrimary(0, 0, 1920, 1040);
// A laptop panel to the right of the primary, shorter than the saved window.
const QRect kSecond(1920, 0, 1280, 800);

} // namespace

TEST_CASE("a32 MW-11: a window saved on a second monitor is restored there", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    window->screenAreas = [] {
        return std::vector<QRect>{kPrimary, kSecond};
    };
    window->availableScreenRect = [] {
        return kPrimary;
    };
    shell.settings().save_setting(
        fv::keys::window_geometry.key(),
        savedAt(kSecond.x() + 100, 40, fv::window_size::kDefaultWidth, 700));
    window->restoreWindowGeometry();
    CHECK(window->x() == kSecond.x() + 100);
    CHECK(window->y() == 40);
    CHECK(window->width() == fv::window_size::kDefaultWidth);
    CHECK(kSecond.contains(window->geometry()));
}

TEST_CASE("a32 MW-11: a saved height taller than a 1280x800 screen is clamped to it", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    window->screenAreas = [] {
        return std::vector<QRect>{kPrimary, kSecond};
    };
    shell.settings().save_setting(
        fv::keys::window_geometry.key(),
        savedAt(kSecond.x() + 100, 300, fv::window_size::kDefaultWidth, 1000));
    window->restoreWindowGeometry();
    // max(minimum height, min(saved, screen)), then moved up to stay on the screen.
    CHECK(window->height() == std::max(window->minimumHeight(), kSecond.height()));
    CHECK(window->height() <= kSecond.height());
    CHECK(window->y() == kSecond.y());
    CHECK(window->x() == kSecond.x() + 100);
    CHECK(kSecond.contains(window->geometry()));
}

TEST_CASE("a32 MW-11: a window saved on a monitor that is gone is centred on the screen",
          "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    window->screenAreas = [] {
        return std::vector<QRect>{kPrimary};
    };
    window->availableScreenRect = [] {
        return kPrimary;
    };
    shell.settings().save_setting(
        fv::keys::window_geometry.key(),
        savedAt(kSecond.x() + 100, 40, fv::window_size::kDefaultWidth, 700));
    window->restoreWindowGeometry();
    CHECK(window->x() == kPrimary.x() + (kPrimary.width() - window->width()) / 2);
    CHECK(window->y() == kPrimary.y() + (kPrimary.height() - window->height()) / 2);
    CHECK(kPrimary.contains(window->geometry()));
}
