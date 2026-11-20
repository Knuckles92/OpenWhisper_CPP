// startSnap and SnapSession (tests/test_snap_session.py, the 33 functions that are not about one
// platform's plumbing, and tests/test_snap_click_screens.py's Windows case). No test grabs the
// real screen: every capture goes through the fake ScreenGrabber of ScopedFakeDesktop, which
// also poses as Windows, and several screens are the offscreen platform's (two_screens.h).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QImage>
#include <QPointer>
#include <QScreen>
#include <QTest>
#include <QWidget>

#include "ui_snap/affinity.h"
#include "ui_snap/annotations.h"
#include "ui_snap/capture.h"
#include "ui_snap/overlay.h"
#include "ui_snap/session.h"
#include "ui_snap/support.h"
#include "ui_snap/testing/fake_desktop.h"
#include "ui_snap/toolbar.h"

#include "overlay_test_support.h"
#include "two_screens.h"

using namespace fv::ui_snap;
using namespace fvtest;
using fv::ui_snap::testing::FakeScreenGrabber;
using fv::ui_snap::testing::ScopedFakeDesktop;

namespace {

using namespace std::chrono_literals;

// The fixture's grabs: fake captures that record which screen was grabbed and which of our
// windows were on screen at that moment, on a desktop posing as Windows. Live sessions are
// dropped at the end, as the Python fixture does.
class Desktop {
public:
    Desktop() : Desktop("win32", QStringLiteral("windows")) {}
    Desktop(std::string system, QString qtPlatform)
        : fake(std::move(system), std::move(qtPlatform)) {
        fake.grabber.image = [this](QScreen& screen) -> std::optional<QImage> {
            QList<QWidget*> visible;
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (widget->isVisible()) {
                    visible.append(widget);
                }
            }
            visibleAtGrab.append(visible);
            if (image) {
                return image(screen);
            }
            return FakeScreenGrabber::screenImage(screen);
        };
    }
    ~Desktop() {
        for (SnapSession* session : SnapSession::liveSessions()) {
            if (session->isActive()) {
                session->drop();
            } else if (session->parent() == nullptr) {
                // A failure still waiting to be reported belongs to its own test.
                delete session;
            }
        }
        flushDeletes();
    }
    Desktop(const Desktop&) = delete;
    Desktop& operator=(const Desktop&) = delete;

    const std::vector<QScreen*>& grabs() const { return fake.grabber.calls; }

    ScopedFakeDesktop fake;
    QList<QList<QWidget*>> visibleAtGrab;
    std::function<std::optional<QImage>(QScreen&)> image;
};

// The fixture's window: a small shown top-level standing in for the recording HUD.
std::unique_ptr<QWidget> hudWindow() {
    auto widget = std::make_unique<QWidget>();
    widget->setWindowTitle(QStringLiteral("Recording HUD"));
    widget->resize(120, 40);
    widget->show();
    return widget;
}

QList<QPointer<QWidget>> keepOut(std::initializer_list<QWidget*> windows) {
    QList<QPointer<QWidget>> listed;
    for (QWidget* window : windows) {
        listed.append(window);
    }
    return listed;
}

QScreen* primary() {
    return QGuiApplication::primaryScreen();
}

// The Parked logger of the Python tests: park and unpark, with the area parked in.
struct ParkLog {
    std::vector<std::pair<std::string, QRect>> entries;

    Parkable parkable() {
        return {[this](QRect area) { entries.emplace_back("park", area); },
                [this] { entries.emplace_back("unpark", QRect()); },
                {}};
    }
};

bool anOverlayIsVisible() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (qobject_cast<SnapOverlay*>(widget) != nullptr && widget->isVisible()) {
            return true;
        }
    }
    return false;
}

const QRect kLeft(0, 0, 800, 600);
const QRect kRight(800, 0, 640, 480);

} // namespace

// ---- TestOutcomes ----

// from
// tests/test_snap_session.py::TestOutcomes::test_a_failed_capture_is_reported_after_start_snap_returns
TEST_CASE("a failed capture is reported after start snap returns", "[test_snap_session]") {
    Desktop desktop;
    desktop.image = [](QScreen&) {
        return std::optional<QImage>();
    };

    QPointer<SnapSession> snap = startSnap();
    Spy<QString> failed(snap.data(), &SnapSession::failed);

    CHECK(failed.size() == 0);
    CHECK_FALSE(snap->isActive());
    CHECK(pumpUntil([&] { return failed.size() == 1; }));
    pump(50ms);
    REQUIRE(failed.size() == 1);
    CHECK(std::get<0>(failed.calls[0]) == QString::fromUtf16(kCaptureFailed));
}

// from tests/test_snap_session.py::TestOutcomes::test_a_capture_that_raises_is_a_failure
TEST_CASE("a capture that raises is a failure", "[test_snap_session]") {
    Desktop desktop;
    desktop.fake.grabber.fail = true;

    SnapSession* snap = startSnap();
    Spy<QString> failed(snap, &SnapSession::failed);

    CHECK(pumpUntil([&] { return failed.size() == 1; }));
    REQUIRE(failed.size() == 1);
    CHECK(std::get<0>(failed.calls[0]) == QString::fromUtf16(kCaptureFailed));
}

// from
// tests/test_snap_session.py::TestOutcomes::test_an_unsupported_desktop_fails_with_its_reason_without_capturing
TEST_CASE("an unsupported desktop fails with its reason without capturing", "[test_snap_session]") {
    Desktop desktop("freebsd14", QStringLiteral("xcb"));

    SnapSession* snap = startSnap();
    Spy<QString> failed(snap, &SnapSession::failed);

    CHECK(failed.size() == 0);
    CHECK(pumpUntil([&] { return failed.size() == 1; }));
    REQUIRE(failed.size() == 1);
    CHECK(std::get<0>(failed.calls[0]) == QString::fromUtf16(kOtherReason));
    CHECK(desktop.grabs().empty());
}

// from tests/test_snap_session.py::TestOutcomes::test_the_overlay_opens_on_the_grabbed_screen
TEST_CASE("the overlay opens on the grabbed screen", "[test_snap_session]") {
    Desktop desktop;
    SnapSession* snap = startSnap({.hint = QStringLiteral("Snap 2")});

    SnapOverlay* overlay = snap->overlay();
    REQUIRE(overlay != nullptr);
    CHECK(overlay->isVisible());
    CHECK(snap->isActive());
    REQUIRE_FALSE(desktop.grabs().empty());
    CHECK(desktop.grabs().front() == overlay->snapScreen());
    CHECK(overlay->windowTitle() == QStringLiteral("OpenWhisper_CPP Snap"));
}

// from tests/test_snap_session.py::TestOutcomes::test_draw_starts_with_the_pen_on_the_whole_screen
TEST_CASE("draw starts with the pen on the whole screen", "[test_snap_session]") {
    Desktop desktop;
    SnapSession* snap = startSnap({.take = SnapTake::Draw});
    SnapOverlay* overlay = snap->overlay();
    REQUIRE(overlay != nullptr);
    CHECK(desktop.grabs().size() == 1);
    CHECK(overlay->selection()->size() == overlay->snapScreen()->geometry().size());
    CHECK(overlay->tool() == SnapTool::Pen);

    Spy<QImage> kept(snap, &SnapSession::kept);
    drag(overlay, {60, 90}, {120, 130});
    QTest::keyClick(overlay, Qt::Key_Return);
    REQUIRE(kept.size() == 1);
    // The pen line runs through (90, 110), which is no longer the captured blue.
    CHECK(std::get<0>(kept.calls[0]).pixelColor(90, 110) != QColor(QStringLiteral("#336699")));
}

// from
// tests/test_snap_session.py::TestOutcomes::test_automatic_snap_targets_the_click_position_without_an_overlay
TEST_CASE("automatic snap targets the click position without an overlay", "[test_snap_session]") {
    Desktop desktop;
    const QPoint origin = primary()->geometry().topLeft();
    desktop.fake.win32.monitor =
        fv::ui_snap::NativeRect{origin.x(), origin.y(), origin.x() + 1920, origin.y() + 1080};

    SnapSession* snap = startSnap({.take = SnapTake::Screen, .clickAt = QPoint(-500, 100)});
    Spy<QImage> kept(snap, &SnapSession::kept);

    CHECK(pumpUntil([&] { return kept.size() == 1; }));
    REQUIRE(desktop.fake.win32.monitorQueries.size() == 1);
    CHECK(desktop.fake.win32.monitorQueries[0] == std::pair{QPoint(-500, 100), false});
    CHECK(desktop.grabs() == std::vector<QScreen*>{primary()});
    CHECK(snap->overlay() == nullptr);
}

// from
// tests/test_snap_session.py::TestOutcomes::test_keep_emits_the_whole_screen_once_and_deletes_the_overlay
TEST_CASE("keep emits the whole screen once and deletes the overlay", "[test_snap_session]") {
    Desktop desktop;
    SnapSession* snap = startSnap();
    Spy<QImage> kept(snap, &SnapSession::kept);
    Spy<> dropped(snap, &SnapSession::dropped);
    QPointer<SnapOverlay> overlay = snap->overlay();
    REQUIRE(overlay);
    QScreen* screen = overlay->snapScreen();

    snap->keep();
    snap->keep();
    snap->drop();

    REQUIRE(kept.size() == 1);
    CHECK(std::get<0>(kept.calls[0]).size() == screen->geometry().size());
    CHECK(dropped.size() == 0);
    CHECK_FALSE(snap->isActive());
    CHECK(snap->captureRect() == screen->geometry());
    CHECK(overlay->isHidden());
    flushDeletes();
    CHECK(overlay.isNull());
}

// from tests/test_snap_session.py::TestOutcomes::test_drop_emits_once
TEST_CASE("drop emits once", "[test_snap_session]") {
    Desktop desktop;
    SnapSession* snap = startSnap();
    Spy<QImage> kept(snap, &SnapSession::kept);
    Spy<> dropped(snap, &SnapSession::dropped);

    snap->drop();
    snap->drop();
    snap->keep();

    CHECK(dropped.size() == 1);
    CHECK(kept.size() == 0);
}

// from tests/test_snap_session.py::TestOutcomes::test_keys_in_the_overlay_reach_the_session
TEST_CASE("keys in the overlay reach the session", "[test_snap_session]") {
    Desktop desktop;
    SnapSession* snap = startSnap();
    Spy<QImage> kept(snap, &SnapSession::kept);

    QTest::keyClick(snap->overlay(), Qt::Key_Return);

    CHECK(kept.size() == 1);
    CHECK_FALSE(snap->isActive());
}

// from tests/test_snap_session.py::TestOutcomes::test_a_session_nobody_holds_still_reports
TEST_CASE("a session nobody holds still reports", "[test_snap_session]") {
    Desktop desktop;
    std::vector<QImage> kept;
    QObject::connect(startSnap(), &SnapSession::kept,
                     [&kept](const QImage& image) { kept.push_back(image); });

    std::vector<SnapSession*> active;
    for (SnapSession* session : SnapSession::liveSessions()) {
        if (session->isActive()) {
            active.push_back(session);
        }
    }
    REQUIRE(active.size() == 1);
    SnapSession* live = active.front();
    live->keep();

    CHECK(kept.size() == 1);
    CHECK_FALSE(SnapSession::liveSessions().contains(live));
}

// from
// tests/test_snap_session.py::TestOutcomes::test_an_owner_deleted_mid_snap_takes_the_overlay_with_it
TEST_CASE("an owner deleted mid snap takes the overlay with it", "[test_snap_session]") {
    Desktop desktop;
    auto* owner = new QObject;
    SnapSession* snap = startSnap({.parent = owner});
    QPointer<SnapOverlay> overlay = snap->overlay();
    REQUIRE(overlay);

    delete owner;
    flushDeletes();

    CHECK(overlay.isNull());
}

// from tests/test_snap_session.py::TestOutcomes::test_start_snap_refuses_other_threads
TEST_CASE("start snap refuses other threads", "[test_snap_session]") {
    Desktop desktop;
    int errors = 0;

    std::thread worker([&errors] {
        try {
            startSnap();
        } catch (const std::logic_error&) {
            ++errors;
        }
    });
    worker.join();

    CHECK(errors == 1);
    CHECK(SnapSession::liveSessions().isEmpty());
}

// from tests/test_snap_session.py::TestOutcomes::test_the_package_exports_the_public_api
TEST_CASE("the package exports the public api", "[test_snap_session]") {
    // OpenWhisper's ui_qt.snap exports SnapSession, keep_out_of_screenshots, snap_support and
    // start_snap; here they are the public headers' declarations.
    static_assert(std::is_base_of_v<QObject, SnapSession>);
    static_assert(std::is_same_v<decltype(&startSnap), SnapSession* (*)(SnapOptions)>);
    static_assert(std::is_same_v<decltype(&keepOutOfScreenshots), bool (*)(QWidget*, bool)>);
    static_assert(
        std::is_same_v<decltype(static_cast<std::pair<bool, QString> (*)()>(&snapSupport)),
                       std::pair<bool, QString> (*)()>);
    CHECK(QString::fromUtf16(kNoScreen) == QStringLiteral("There's no screen to capture."));
    CHECK(QString::fromUtf16(kCaptureFailed) ==
          QStringLiteral("Couldn't capture the screen. Try again in a moment."));
}

// ---- TestKeepOut ----

// from tests/test_snap_session.py::TestKeepOut::test_windows_are_hidden_for_the_grab_then_restored
TEST_CASE("windows are hidden for the grab then restored", "[test_snap_session]") {
    Desktop desktop;
    const auto window = hudWindow();

    SnapSession* snap = startSnap({.keepOut = keepOut({window.get()})});

    // Hidden now, captured after a couple of frames, never inside startSnap.
    CHECK_FALSE(window->isVisible());
    CHECK(desktop.grabs().empty());
    CHECK(pumpUntil([&] { return snap->overlay() != nullptr; }));
    REQUIRE_FALSE(desktop.visibleAtGrab.isEmpty());
    CHECK_FALSE(desktop.visibleAtGrab[0].contains(window.get()));
    CHECK(window->isVisible());
    snap->drop();
}

// from tests/test_snap_session.py::TestKeepOut::test_windows_kept_out_by_affinity_stay_on_screen
TEST_CASE("windows kept out by affinity stay on screen", "[test_snap_session]") {
    Desktop desktop;
    const auto window = hudWindow();
    REQUIRE(keepOutOfScreenshots(window.get(), true));

    SnapSession* snap = startSnap({.keepOut = keepOut({window.get()})});

    CHECK(desktop.grabs().size() == 1);
    REQUIRE_FALSE(desktop.visibleAtGrab.isEmpty());
    CHECK(desktop.visibleAtGrab[0].contains(window.get()));
    CHECK(window->isVisible());
    snap->drop();
}

// from tests/test_snap_session.py::TestKeepOut::test_hidden_and_minimised_windows_are_left_alone
TEST_CASE("hidden and minimised windows are left alone", "[test_snap_session]") {
    Desktop desktop;
    const auto window = hudWindow();
    QWidget hidden;
    window->showMinimized();

    SnapSession* snap = startSnap({.keepOut = keepOut({&hidden, window.get(), nullptr})});

    CHECK(desktop.grabs().size() == 1);
    CHECK_FALSE(hidden.isVisible());
    snap->drop();
}

// from
// tests/test_snap_session.py::TestKeepOut::test_drop_while_waiting_restores_windows_without_capturing
TEST_CASE("drop while waiting restores windows without capturing", "[test_snap_session]") {
    Desktop desktop;
    const auto window = hudWindow();
    SnapSession* snap = startSnap({.keepOut = keepOut({window.get()})});
    Spy<> dropped(snap, &SnapSession::dropped);

    snap->drop();

    CHECK(dropped.size() == 1);
    CHECK(window->isVisible());
    pump(250ms);
    CHECK(desktop.grabs().empty());
}

// from
// tests/test_snap_session.py::TestKeepOut::test_keep_while_waiting_keeps_the_whole_screen_once_captured
TEST_CASE("keep while waiting keeps the whole screen once captured", "[test_snap_session]") {
    Desktop desktop;
    const auto window = hudWindow();
    SnapSession* snap = startSnap({.keepOut = keepOut({window.get()})});
    Spy<QImage> kept(snap, &SnapSession::kept);

    snap->keep();

    CHECK(kept.size() == 0);
    CHECK(pumpUntil([&] { return kept.size() == 1; }));
    REQUIRE(kept.size() == 1);
    CHECK_FALSE(anOverlayIsVisible());
    CHECK(std::get<0>(kept.calls[0]).size() == primary()->geometry().size());
    CHECK(window->isVisible());
}

// from
// tests/test_snap_session.py::TestKeepOut::test_an_owner_deleted_while_waiting_shows_the_windows_again
TEST_CASE("an owner deleted while waiting shows the windows again", "[test_snap_session]") {
    Desktop desktop;
    const auto window = hudWindow();
    auto* owner = new QObject;
    startSnap({.keepOut = keepOut({window.get()}), .parent = owner});
    REQUIRE_FALSE(window->isVisible());

    delete owner;

    CHECK(window->isVisible());
    pump(250ms);
    CHECK(desktop.grabs().empty());
}

// from tests/test_snap_session.py::TestKeepOut::test_one_window_listed_twice_is_hidden_once
TEST_CASE("one window listed twice is hidden once", "[test_snap_session]") {
    Desktop desktop;
    const auto window = hudWindow();

    const QList<QPointer<QWidget>> hidden = hideWindows(keepOut({window.get(), window.get()}));

    REQUIRE(hidden.size() == 1);
    CHECK(hidden[0] == window.get());
    restoreWindows(hidden);
    CHECK(window->isVisible());
}

// ---- TestFloatAbove ----

// from
// tests/test_snap_session.py::TestFloatAbove::test_windows_park_over_the_open_overlay_and_return_before_the_outcome
TEST_CASE("windows park over the open overlay and return before the outcome",
          "[test_snap_session]") {
    Desktop desktop;
    ParkLog log;
    SnapSession* snap = startSnap({.floatAbove = {log.parkable()}});
    QObject::connect(snap, &SnapSession::kept,
                     [&log](const QImage&) { log.entries.emplace_back("kept", QRect()); });

    REQUIRE(snap->overlay() != nullptr);
    CHECK(log.entries == std::vector<std::pair<std::string, QRect>>{
                             {"park", snap->overlay()->snapScreen()->geometry()}});
    snap->keep();

    REQUIRE(log.entries.size() == 3);
    CHECK(log.entries[1].first == "unpark");
    CHECK(log.entries[2].first == "kept");
}

// from tests/test_snap_session.py::TestFloatAbove::test_a_snap_that_never_opens_parks_nothing
TEST_CASE("a snap that never opens parks nothing", "[test_snap_session]") {
    Desktop desktop;
    desktop.image = [](QScreen&) {
        return std::optional<QImage>();
    };
    ParkLog log;

    QPointer<SnapSession> snap = startSnap({.floatAbove = {log.parkable()}});

    CHECK(pumpUntil([] { return SnapSession::liveSessions().isEmpty(); }));
    CHECK((snap.isNull() || !snap->isActive()));
    CHECK(log.entries.empty());
}

// from
// tests/test_snap_session.py::TestFloatAbove::test_an_owner_deleted_mid_snap_puts_the_windows_back
TEST_CASE("an owner deleted mid snap puts the windows back", "[test_snap_session]") {
    Desktop desktop;
    ParkLog log;
    auto* owner = new QObject;
    startSnap({.floatAbove = {log.parkable()}, .parent = owner});

    delete owner;

    REQUIRE(log.entries.size() == 2);
    CHECK(log.entries[0].first == "park");
    CHECK(log.entries[1].first == "unpark");
}

TEST_CASE("a parked window that was deleted is not called back", "[test_snap_session]") {
    Desktop desktop;
    ParkLog log;
    auto* window = new QObject;
    Parkable parkable = log.parkable();
    parkable.window = window;
    SnapSession* snap = startSnap({.floatAbove = {parkable}});
    REQUIRE(log.entries.size() == 1);

    delete window;
    snap->drop();

    CHECK(log.entries.size() == 1);
}

TEST_CASE("an owner deleted by the outcome's receiver leaves nothing behind",
          "[test_snap_session]") {
    const bool failing = GENERATE(false, true);
    CAPTURE(failing);
    Desktop desktop;
    if (failing) {
        desktop.image = [](QScreen&) {
            return std::optional<QImage>();
        };
    }
    QPointer<QObject> owner = new QObject;
    ParkLog log;
    SnapSession* snap = startSnap({.floatAbove = {log.parkable()}, .parent = owner.data()});
    QPointer<SnapOverlay> overlay = snap->overlay();
    int outcomes = 0;
    QObject::connect(snap, &SnapSession::kept, [&outcomes, &owner](const QImage&) {
        ++outcomes;
        delete owner.data();
    });
    QObject::connect(snap, &SnapSession::failed, [&outcomes, &owner](const QString&) {
        ++outcomes;
        delete owner.data();
    });

    if (failing) {
        CHECK(pumpUntil([&] { return outcomes == 1; }));
    } else {
        REQUIRE(overlay);
        snap->keep();
    }
    flushDeletes();

    CHECK(outcomes == 1);
    CHECK(owner.isNull());
    CHECK(SnapSession::liveSessions().isEmpty());
    CHECK(overlay.isNull());
    if (!failing) {
        CHECK(log.entries.size() == 2);
    }
}

// ---- TestEveryScreen ----

// from tests/test_snap_session.py::TestEveryScreen::test_each_screen_is_captured_and_covered
TEST_CASE("each screen is captured and covered", "[test_snap_session]") {
    ScopedScreens screens({kLeft, kRight});
    REQUIRE(screens.available());
    Desktop desktop;
    QCursor::setPos(kRight.center());
    const QList<QScreen*> all = QGuiApplication::screens();

    SnapSession* snap = startSnap();

    // The pointer's screen first.
    CHECK(desktop.grabs() == std::vector<QScreen*>{all[1], all[0]});
    const QList<SnapOverlay*> overlays = snap->overlays();
    REQUIRE(overlays.size() == 2);
    for (SnapOverlay* overlay : overlays) {
        CHECK(overlay->isVisible());
    }
    CHECK(overlays[0]->snapScreen() == all[1]);
    CHECK(overlays[0]->geometry() == kRight);
    CHECK(overlays[1]->geometry() == kLeft);
}

// from
// tests/test_snap_session.py::TestEveryScreen::test_infill_choice_follows_the_user_to_another_screen
TEST_CASE("infill choice follows the user to another screen", "[test_snap_session]") {
    ScopedScreens screens({kLeft, kRight});
    REQUIRE(screens.available());
    Desktop desktop;
    SnapSession* snap = startSnap();
    const QList<SnapOverlay*> overlays = snap->overlays();
    REQUIRE(overlays.size() == 2);
    SnapOverlay* first = overlays[0];
    SnapOverlay* second = overlays[1];

    first->startDrawing();
    first->setTool(SnapTool::Box);
    click(first->toolbar(), center(first->toolbar()->buttonRect(QStringLiteral("infill"))));
    CHECK(second->toolbar()->boxInfill());
    second->startDrawing();
    second->setTool(SnapTool::Box);
    drag(second, {80, 60}, {180, 120});
    REQUIRE(second->annotations().size() > 0);
    CHECK(second->annotations().items().back().infill);
    click(second->toolbar(), center(second->toolbar()->buttonRect(QStringLiteral("infill"))));
    CHECK_FALSE(first->toolbar()->boxInfill());
}

// from
// tests/test_snap_session.py::TestEveryScreen::test_selecting_on_another_screen_moves_the_selection_there
TEST_CASE("selecting on another screen moves the selection there", "[test_snap_session]") {
    ScopedScreens screens({kLeft, kRight});
    REQUIRE(screens.available());
    Desktop desktop;
    SnapSession* snap = startSnap();
    Spy<QImage> kept(snap, &SnapSession::kept);
    const QList<SnapOverlay*> overlays = snap->overlays();
    REQUIRE(overlays.size() == 2);
    SnapOverlay* first = overlays[0];
    SnapOverlay* second = overlays[1];
    first->selectAll();

    drag(second, {18, 18}, {118, 68});

    CHECK_FALSE(first->selection().has_value());
    CHECK(second->selection() == QRect(18, 18, 100, 50));
    CHECK(snap->overlay() == second);
    const QPoint origin = second->pos();
    snap->keep();
    REQUIRE(kept.size() == 1);
    CHECK(std::get<0>(kept.calls[0]).size() == QSize(100, 50));
    CHECK(snap->captureRect() == QRect(18, 18, 100, 50).translated(origin));
    CHECK_FALSE(first->isVisible());
    CHECK_FALSE(second->isVisible());
}

// from tests/test_snap_session.py::TestEveryScreen::test_esc_on_any_screen_drops_the_whole_snap
TEST_CASE("esc on any screen drops the whole snap", "[test_snap_session]") {
    ScopedScreens screens({kLeft, kRight});
    REQUIRE(screens.available());
    Desktop desktop;
    SnapSession* snap = startSnap();
    Spy<> dropped(snap, &SnapSession::dropped);
    const QList<SnapOverlay*> overlays = snap->overlays();
    REQUIRE(overlays.size() == 2);

    QTest::keyClick(overlays[1], Qt::Key_Escape);

    CHECK(dropped.size() == 1);
    CHECK_FALSE(overlays[0]->isVisible());
    CHECK_FALSE(overlays[1]->isVisible());
}

// from
// tests/test_snap_session.py::TestEveryScreen::test_a_screen_that_fails_to_capture_is_left_uncovered
TEST_CASE("a screen that fails to capture is left uncovered", "[test_snap_session]") {
    ScopedScreens screens({kLeft, kRight});
    REQUIRE(screens.available());
    Desktop desktop;
    int calls = 0;
    desktop.image = [&calls](QScreen& screen) -> std::optional<QImage> {
        if (calls++ == 0) {
            return FakeScreenGrabber::screenImage(screen);
        }
        return std::nullopt;
    };

    SnapSession* snap = startSnap();

    CHECK(snap->overlays().size() == 1);
    snap->drop();
}

// from
// tests/test_snap_session.py::TestEveryScreen::test_the_pointers_screen_comes_first_and_hyprland_covers_only_it
TEST_CASE("the pointers screen comes first and hyprland covers only it", "[test_snap_session]") {
    // The Hyprland half (one screen through grim) is other-platform: Hyprland is not ported.
    ScopedScreens screens(
        {QRect(0, 0, 640, 480), QRect(640, 0, 640, 480), QRect(1280, 0, 640, 480)});
    REQUIRE(screens.available());
    Desktop desktop;
    const QList<QScreen*> all = QGuiApplication::screens();
    QCursor::setPos(QPoint(900, 200));

    CHECK(screensToCover() == QList<QScreen*>{all[1], all[0], all[2]});
}

TEST_CASE("a snap over two screens reports one outcome, after it returned, with the marks",
          "[test_snap_session]") {
    ScopedScreens screens({kLeft, kRight});
    REQUIRE(screens.available());
    Desktop desktop;
    desktop.image = [](QScreen& screen) {
        return std::optional<QImage>(FakeScreenGrabber::screenImage(screen, 1.5));
    };
    int outcomes = 0;
    SnapSession* snap = startSnap();
    QObject::connect(snap, &SnapSession::kept, [&outcomes](const QImage&) { ++outcomes; });
    QObject::connect(snap, &SnapSession::dropped, [&outcomes] { ++outcomes; });
    QObject::connect(snap, &SnapSession::failed, [&outcomes](const QString&) { ++outcomes; });
    Spy<QImage> kept(snap, &SnapSession::kept);
    REQUIRE(snap->overlays().size() == 2);
    SnapOverlay* second = snap->overlays()[1];

    drag(second, {100, 100}, {300, 200});
    second->setTool(SnapTool::Box);
    drag(second, {120, 120}, {200, 160});
    QTest::keyClick(second, Qt::Key_Return);
    snap->drop();
    pump(50ms);

    CHECK(outcomes == 1);
    REQUIRE(kept.size() == 1);
    const QImage& image = std::get<0>(kept.calls[0]);
    CHECK(image.size() == QSize(300, 150));
    CHECK(image.devicePixelRatio() == 1.5);
    // The box's left edge, 20 logical pixels into the crop at 1.5x.
    const QColor edge = image.pixelColor(30, 60);
    CHECK(edge.red() > 200);
    CHECK(edge.green() < 140);
}

TEST_CASE("a screen unplugged before the grab is not grabbed", "[test_snap_session]") {
    ScopedScreens screens({kLeft, kRight});
    REQUIRE(screens.available());
    Desktop desktop;
    const auto window = hudWindow();
    QCursor::setPos(kRight.center());
    SnapSession* snap = startSnap({.keepOut = keepOut({window.get()})});
    Spy<QString> failed(snap, &SnapSession::failed);
    REQUIRE_FALSE(window->isVisible());
    REQUIRE(desktop.grabs().empty());

    // The pointer's screen goes away while the settle timer waits.
    ScopedScreens fewer({kLeft});
    REQUIRE(fewer.available());

    CHECK(pumpUntil([&] { return failed.size() == 1; }));
    REQUIRE(failed.size() == 1);
    CHECK(std::get<0>(failed.calls[0]) == QString::fromUtf16(kCaptureFailed));
    for (QScreen* grabbed : desktop.grabs()) {
        CHECK(QGuiApplication::screens().contains(grabbed));
    }
    CHECK(window->isVisible());
}

// ---- TestKeepingAtOnce ----

// from
// tests/test_snap_session.py::TestKeepingAtOnce::test_the_whole_screen_is_kept_after_start_snap_returns
TEST_CASE("the whole screen is kept after start snap returns", "[test_snap_session]") {
    Desktop desktop;
    SnapSession* snap = startSnap({.take = SnapTake::Screen});
    Spy<QImage> kept(snap, &SnapSession::kept);

    CHECK(kept.size() == 0);
    CHECK(snap->isActive());
    CHECK(pumpUntil([&] { return kept.size() == 1; }));
    REQUIRE(kept.size() == 1);
    CHECK(std::get<0>(kept.calls[0]).deviceIndependentSize().toSize() ==
          screenUnderCursor()->geometry().size());
    CHECK_FALSE(anOverlayIsVisible());
}

// from
// tests/test_snap_session.py::TestKeepingAtOnce::test_the_window_under_the_pointer_is_cropped_out_of_its_screen
TEST_CASE("the window under the pointer is cropped out of its screen", "[test_snap_session]") {
    Desktop desktop;
    const QPoint origin = primary()->geometry().topLeft();
    desktop.fake.win32.monitor =
        fv::ui_snap::NativeRect{origin.x(), origin.y(), origin.x() + 800, origin.y() + 600};
    desktop.fake.win32.foreground = 42;
    desktop.fake.lister.frames[42] =
        fv::ui_snap::NativeRect{origin.x() + 10, origin.y() + 20, origin.x() + 74, origin.y() + 68};

    SnapSession* snap = startSnap({.take = SnapTake::Window, .front = true});
    Spy<QImage> kept(snap, &SnapSession::kept);

    CHECK(pumpUntil([&] { return kept.size() == 1; }));
    REQUIRE(kept.size() == 1);
    CHECK(std::get<0>(kept.calls[0]).deviceIndependentSize().toSize() == QSize(64, 48));
    // front: the foreground window, never the one under the pointer.
    CHECK(desktop.fake.lister.pointQueries.empty());
    CHECK(snap->captureRect() == QRect(10, 20, 64, 48).translated(origin));
}

// from
// tests/test_snap_session.py::TestKeepingAtOnce::test_a_window_that_cant_be_found_leaves_the_whole_screen
TEST_CASE("a window that cant be found leaves the whole screen", "[test_snap_session]") {
    Desktop desktop;

    SnapSession* snap = startSnap({.take = SnapTake::Window});
    Spy<QImage> kept(snap, &SnapSession::kept);

    CHECK(pumpUntil([&] { return kept.size() == 1; }));
    REQUIRE(kept.size() == 1);
    CHECK(std::get<0>(kept.calls[0]).deviceIndependentSize().toSize() ==
          screenUnderCursor()->geometry().size());
}

// from tests/test_snap_session.py::TestKeepingAtOnce::test_only_the_pointers_screen_is_grabbed
TEST_CASE("only the pointers screen is grabbed", "[test_snap_session]") {
    ScopedScreens screens({kLeft, kRight});
    REQUIRE(screens.available());
    Desktop desktop;
    QCursor::setPos(kRight.center());

    SnapSession* snap = startSnap({.take = SnapTake::Screen});
    Spy<QImage> kept(snap, &SnapSession::kept);

    CHECK(pumpUntil([&] { return kept.size() == 1; }));
    CHECK(desktop.grabs() == std::vector<QScreen*>{QGuiApplication::screens()[1]});
}

// ---- tests/test_snap_click_screens.py ----

// from tests/test_snap_click_screens.py::test_windows_click_keeps_native_monitor_mapping
TEST_CASE("windows click keeps native monitor mapping", "[test_snap_click_screens]") {
    ScopedScreens screens({QRect(0, 0, 800, 600), QRect(-1920, 0, 1920, 1080)});
    REQUIRE(screens.available());
    Desktop desktop;
    desktop.fake.win32.monitor = fv::ui_snap::NativeRect{-1920, 0, 0, 1080};

    CHECK(screenAtClick(QPoint(-1800, 400)) == QGuiApplication::screens()[1]);
    REQUIRE(desktop.fake.win32.monitorQueries.size() == 1);
    CHECK(desktop.fake.win32.monitorQueries[0] == std::pair{QPoint(-1800, 400), false});
}
