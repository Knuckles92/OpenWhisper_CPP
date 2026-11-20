// Capture exclusion and foreground forcing over the fake Win32 desktop
// (tests/test_snap_session.py::TestAffinity, and the foreground calls of affinity.py).
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include <QWidget>

#include "ui_snap/affinity.h"
#include "ui_snap/testing/fake_desktop.h"

using namespace fv::ui_snap;
using fv::ui_snap::testing::FakeWin32Desktop;
using fv::ui_snap::testing::ScopedFakeDesktop;

namespace {

// A window whose native handle a test can throw away and make again, as Qt does when a
// window's flags or translucency change.
class Recreatable : public QWidget {
public:
    void recreate() {
        destroy(true, true);
        create();
    }
};

// The fixture's window: a small shown top-level, as the recording HUD is.
std::unique_ptr<Recreatable> hud() {
    auto window = std::make_unique<Recreatable>();
    window->setWindowTitle(QStringLiteral("Recording HUD"));
    window->resize(120, 40);
    window->show();
    return window;
}

} // namespace

// from tests/test_snap_session.py::TestAffinity::test_excludes_then_restores_the_window
TEST_CASE("excludes then restores the window", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    const auto window = hud();
    const WId hwnd = window->winId();

    CHECK(keepOutOfScreenshots(window.get(), true));
    CHECK(isKeptOut(window.get()));
    CHECK(keepOutOfScreenshots(window.get(), false));
    CHECK_FALSE(isKeptOut(window.get()));

    CHECK(desktop.win32.affinityCalls ==
          std::vector<FakeWin32Desktop::AffinityCall>{{hwnd, 0x11}, {hwnd, 0x0}});
}

// from tests/test_snap_session.py::TestAffinity::test_a_refused_exclusion_is_not_remembered
TEST_CASE("a refused exclusion is not remembered", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    desktop.win32.affinityResult = false;
    const auto window = hud();

    CHECK_FALSE(keepOutOfScreenshots(window.get(), true));
    CHECK_FALSE(isKeptOut(window.get()));
}

// from
// tests/test_snap_session.py::TestAffinity::test_a_recreated_native_window_counts_as_not_excluded
TEST_CASE("a recreated native window counts as not excluded", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    const auto window = hud();
    keepOutOfScreenshots(window.get(), true);
    const WId excluded = window->winId();

    window->recreate();

    REQUIRE(window->winId() != excluded);
    CHECK_FALSE(isKeptOut(window.get()));
}

// from tests/test_snap_session.py::TestAffinity::test_a_child_widget_means_its_window
TEST_CASE("a child widget means its window", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    const auto window = hud();
    auto* child = new QWidget(window.get());

    keepOutOfScreenshots(child, true);

    CHECK(desktop.win32.affinityCalls ==
          std::vector<FakeWin32Desktop::AffinityCall>{{window->winId(), 0x11}});
    CHECK(isKeptOut(window.get()));
}

// from tests/test_snap_session.py::TestAffinity::test_elsewhere_it_does_nothing
TEST_CASE("capture exclusion elsewhere does nothing", "[test_snap_session]") {
    ScopedFakeDesktop desktop("linux", QStringLiteral("xcb"));
    const auto window = hud();

    CHECK_FALSE(keepOutOfScreenshots(window.get(), true));
    CHECK(desktop.win32.affinityCalls.empty());
    CHECK_FALSE(isKeptOut(window.get()));
}

TEST_CASE("a failing affinity call is a refusal", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    desktop.win32.affinityThrows = true;
    const auto window = hud();

    CHECK_FALSE(keepOutOfScreenshots(window.get(), true));
    CHECK_FALSE(isKeptOut(window.get()));
}

TEST_CASE("an exclusion is forgotten with its window", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    auto window = hud();
    keepOutOfScreenshots(window.get(), true);
    window.reset();
    // A new window that happens to reuse the address starts out not excluded.
    const auto next = hud();
    CHECK_FALSE(isKeptOut(next.get()));
}

TEST_CASE("bringing to front attaches to the foreground thread for the call",
          "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    desktop.win32.foreground = 7;
    desktop.win32.threads[7] = 70;
    desktop.win32.ourThread = 1;
    const auto overlay = hud();

    bringToFront(overlay.get());

    CHECK(desktop.win32.calls ==
          std::vector<std::string>{"attach 70 1", "foreground " + std::to_string(overlay->winId()),
                                   "detach 70 1"});
}

TEST_CASE("bringing to front from our own thread attaches nothing", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    desktop.win32.foreground = 7;
    desktop.win32.threads[7] = 1;
    desktop.win32.ourThread = 1;
    const auto overlay = hud();

    bringToFront(overlay.get());

    CHECK(desktop.win32.calls ==
          std::vector<std::string>{"foreground " + std::to_string(overlay->winId())});
}

TEST_CASE("a refused attach still asks for the foreground and detaches nothing",
          "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    desktop.win32.foreground = 7;
    desktop.win32.threads[7] = 70;
    desktop.win32.attachResult = false;
    const auto overlay = hud();

    bringToFront(overlay.get());

    CHECK(
        desktop.win32.calls ==
        std::vector<std::string>{"attach 70 1", "foreground " + std::to_string(overlay->winId())});
}

TEST_CASE("the foreground window is left alone", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    const auto overlay = hud();
    desktop.win32.foreground = overlay->winId();

    bringToFront(overlay.get());

    CHECK(desktop.win32.calls.empty());
}

TEST_CASE("bringing to front is only for the windows platform", "[test_snap_session]") {
    ScopedFakeDesktop desktop("win32", QStringLiteral("offscreen"));
    desktop.win32.foreground = 7;
    const auto overlay = hud();

    bringToFront(overlay.get());

    CHECK(desktop.win32.calls.empty());
    CHECK(foregroundWindow() == 0);
}

TEST_CASE("the focus goes back only from our drawing surfaces", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    const auto canvas = hud();
    const auto other = hud();
    const QList<QPointer<QWidget>> ours{canvas.get()};

    desktop.win32.foreground = other->winId();
    returnForeground(99, ours);
    CHECK(desktop.win32.calls.empty());

    desktop.win32.foreground = canvas->winId();
    returnForeground(0, ours);
    CHECK(desktop.win32.calls.empty());

    canvas->hide();
    returnForeground(99, ours);
    CHECK(desktop.win32.calls.empty());

    canvas->show();
    returnForeground(99, ours);
    CHECK(desktop.win32.foreground == 99);
    CHECK(foregroundWindow() == 99);
}
