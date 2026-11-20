// The a32 checklist's main-window claims (MW-06, MW-11, MW-15, MW-16) that no other test pins:
// the Quit button's Ctrl+Q hint, where a saved window comes back, and that every Quit entry
// point asks first and is not vetoed by close-to-tray once confirmed.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QLabel>
#include <QPushButton>
#include <QRect>
#include <QScreen>

#include "appkit/registries.h"
#include "settings/keys.h"
#include "shell_fixture.h"
#include "ui_shell/footer_bar.h"
#include "ui_shell/main_window.h"
#include "ui_widgets/buttons.h"
#include "uimath/window_geometry.h"

using fv::MainWindow;
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

} // namespace

// MW-06: the footer's Quit shows the window's Exit shortcut as its hover hint.
TEST_CASE("a32 MW-06: the Quit button's hint is Ctrl+Q", "[widget]") {
    fv::FooterBar footer;
    footer.show();
    auto* hint = footer.quitButton()->findChild<fv::HotkeyHintFilter*>();
    REQUIRE(hint != nullptr);
    hint->showHint();
    REQUIRE(hint->hint() != nullptr);
    CHECK(hint->hint()->findChild<QLabel*>()->text() == QStringLiteral("Ctrl+Q"));
}

// MW-11: a saved position on a connected screen is where the window comes back; one on no
// screen is ignored and the window is centred (main_window.py _apply_saved_geometry).
TEST_CASE("a32 MW-11: a saved position on screen is restored and one off every screen is ignored",
          "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    // The real screen the restore looks at (QScreen), not the fixture's pinned rectangle.
    const QRect real = QGuiApplication::primaryScreen()->availableGeometry();
    shell.settings().save_setting(
        fv::keys::window_geometry.key(),
        savedAt(real.x() + 30, real.y() + 20, fv::window_size::kDefaultWidth, 700));
    window->restoreWindowGeometry();
    CHECK(window->x() == real.x() + 30);
    // A screen shorter than the window pins it to the top, as _clamp_geometry does.
    CHECK(window->y() ==
          std::min(real.y() + 20, std::max(real.y(), real.y() + real.height() - window->height())));
    CHECK(window->width() == fv::window_size::kDefaultWidth);

    shell.settings().save_setting(fv::keys::window_geometry.key(),
                                  savedAt(-9000, -9000, fv::window_size::kDefaultWidth, 700));
    window->restoreWindowGeometry();
    const QRect screen = window->availableScreenRect();
    CHECK(window->x() == screen.x() + (screen.width() - window->width()) / 2);
    CHECK(screen.contains(window->geometry().topLeft()));
}

// MW-15: the footer's Quit and File > Exit both ask the confirmation (the tray's Exit is
// "tray exit goes through the window" in ui_controller).
TEST_CASE("a32 MW-15: the Quit button and File > Exit both ask before quitting", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    int asked = 0;
    window->confirmQuit = [&asked] {
        ++asked;
        return false;
    };
    window->footer()->quitButton()->click();
    QApplication::processEvents();
    CHECK(asked == 1);
    shell.context().menus().action(fv::menu_id::exit)->trigger();
    QApplication::processEvents();
    CHECK(asked == 2);
}

// MW-16: once a Quit is confirmed, close-to-tray no longer turns the window's close into a hide
// (main_window.py _force_quit). No event loop runs here, so QApplication::quit() does nothing.
TEST_CASE("a32 MW-16: a confirmed Quit is not vetoed by close to tray", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    REQUIRE(shell.settings().get(fv::keys::minimize_tray));
    window->show();
    QApplication::processEvents();
    window->confirmQuit = [] {
        return true;
    };
    window->requestQuit();
    QCloseEvent event;
    QApplication::sendEvent(window.get(), &event);
    CHECK(event.isAccepted());
}
