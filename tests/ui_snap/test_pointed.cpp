// The window under the pointer and the window edges to snap to, over the fake window lister
// (pointed.py; OpenWhisper exercises these through its session tests, which M6.8 ports).
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>

#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QWidget>

#include "ui_snap/pointed.h"
#include "ui_snap/testing/fake_desktop.h"

using namespace fv::ui_snap;
using fv::ui_snap::testing::ScopedFakeDesktop;

namespace {

QScreen* screen() {
    return QGuiApplication::primaryScreen();
}

QPoint origin() {
    return screen()->geometry().topLeft();
}

// A native rect of the given logical geometry on the primary screen, at its pixel ratio.
NativeRect native(int x, int y, int width, int height) {
    const double dpr = screen()->devicePixelRatio();
    const int left = origin().x() + static_cast<int>(x * dpr);
    const int top = origin().y() + static_cast<int>(y * dpr);
    return NativeRect{left, top, left + static_cast<int>(width * dpr),
                      top + static_cast<int>(height * dpr)};
}

} // namespace

TEST_CASE("pointed answers on Windows and macOS only", "[test_snap_session]") {
    {
        ScopedFakeDesktop desktop("win32", QStringLiteral("windows"));
        CHECK(pointedSupported());
    }
    {
        ScopedFakeDesktop desktop("darwin", QStringLiteral("cocoa"));
        CHECK(pointedSupported());
        CHECK(windowTargets(screen()).isEmpty());
        CHECK_FALSE(windowUnderPointer(screen()).has_value());
    }
    {
        ScopedFakeDesktop desktop("linux", QStringLiteral("xcb"));
        CHECK_FALSE(pointedSupported());
        CHECK(windowTargets(screen()).isEmpty());
        CHECK_FALSE(windowUnderPointer(screen()).has_value());
    }
}

TEST_CASE("window targets keep visible frames front to back", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    auto& lister = desktop.lister;
    lister.add(1, native(100, 100, 400, 300));         // in front
    lister.add(2, native(120, 120, 200, 100));         // fully behind window 1
    lister.add(3, native(450, 350, 300, 200));         // partly behind window 1
    lister.add(4, native(10, 10, 23, 400));            // under 24 px wide
    lister.add(5, native(600, 10, 100, 20));           // under 24 px tall
    lister.listed.push_back({6, false, false, false}); // invisible
    lister.frames[6] = native(0, 0, 50, 50);
    lister.listed.push_back({7, true, true, false}); // minimised
    lister.frames[7] = native(0, 0, 50, 50);
    lister.listed.push_back({8, true, false, true}); // cloaked
    lister.frames[8] = native(0, 0, 50, 50);
    lister.listed.push_back({9, true, false, std::nullopt}); // cloak unknown: counts
    lister.frames[9] = native(5, 450, 60, 60);
    lister.listed.push_back({10, true, false, false}); // gone before its frame was read
    lister.add(11, native(0, 0, 1920, 1080));          // the desktop behind everything
    lister.add(12, native(-400, 0, 300, 300));         // on another monitor only

    const QList<QRect> targets = windowTargets(screen());

    // Frames keep their real edges past the screen (a window straddling two monitors).
    CHECK(targets == QList<QRect>{QRect(100, 100, 400, 300), QRect(450, 350, 300, 200),
                                  QRect(5, 450, 60, 60), QRect(0, 0, 1920, 1080)});
}

TEST_CASE("our own windows are no targets", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    QWidget ours;
    ours.resize(200, 100);
    ours.show();
    QWidget hiddenOurs;
    desktop.lister.add(ours.winId(), native(100, 100, 200, 100));
    desktop.lister.add(hiddenOurs.winId(), native(400, 100, 200, 100));

    const QList<QRect> targets = windowTargets(screen(), {&ours, &hiddenOurs, nullptr});

    // A hidden window of ours is not excluded, as in Python: only visible ones are.
    CHECK(targets == QList<QRect>{QRect(400, 100, 200, 100)});
}

TEST_CASE("unreadable window edges are no targets", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    desktop.lister.add(1, native(100, 100, 400, 300));
    desktop.lister.fail = true;

    CHECK(windowTargets(screen()).isEmpty());
}

TEST_CASE("frames convert to logical pixels half to even", "[test_snap_session]") {
    CHECK(logicalFrame(NativeRect{125, 375, 1125, 1000}, QPoint(0, 0), 1.25) ==
          QRect(100, 300, 800, 500));
    CHECK(logicalFrame(NativeRect{3, 9, 6, 12}, QPoint(0, 0), 2.0) == QRect(2, 4, 2, 2));
    CHECK(logicalFrame(NativeRect{1923, -3, 1926, 0}, QPoint(1920, 0), 1.5) == QRect(2, -2, 2, 2));
}

TEST_CASE("the window under the pointer is cut to its screen", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    const QRect area(QPoint(0, 0), screen()->geometry().size());
    desktop.lister.cursor = QPoint(50, 60);
    desktop.lister.underCursor = 42;
    desktop.win32.monitor =
        NativeRect{origin().x(), origin().y(), origin().x() + 4000, origin().y() + 4000};
    desktop.lister.frames[42] = native(-20, 30, 300, 200);

    const std::optional<QRect> found = windowUnderPointer(screen());

    REQUIRE(found.has_value());
    CHECK(*found == QRect(-20, 30, 300, 200).intersected(area));
    CHECK(desktop.lister.pointQueries == std::vector<QPoint>{QPoint(50, 60)});
    REQUIRE(desktop.win32.monitorQueries.size() == 1);
    CHECK(desktop.win32.monitorQueries[0] == std::pair{QPoint(50, 60), true});
}

TEST_CASE("front asks for the window in front", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    desktop.win32.foreground = 9;
    desktop.lister.underCursor = 42;
    desktop.win32.monitor = NativeRect{origin().x(), origin().y(), 0, 0};
    desktop.lister.frames[9] = native(10, 20, 100, 80);
    desktop.lister.frames[42] = native(300, 300, 100, 80);

    CHECK(windowUnderPointer(screen(), {}, true) == QRect(10, 20, 100, 80));
    CHECK(desktop.lister.pointQueries.empty());
}

TEST_CASE("the pointer on one of ours means the window in front", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    QWidget column;
    column.setGeometry(QRect(origin() + QPoint(40, 40), QSize(200, 200)));
    column.show();
    QCursor::setPos(column.frameGeometry().center());
    REQUIRE(column.frameGeometry().contains(QCursor::pos()));
    desktop.win32.foreground = 9;
    desktop.lister.underCursor = 42;
    desktop.win32.monitor = NativeRect{origin().x(), origin().y(), 0, 0};
    desktop.lister.frames[9] = native(10, 20, 100, 80);
    desktop.lister.frames[42] = native(300, 300, 100, 80);

    CHECK(windowUnderPointer(screen(), {&column}) == QRect(10, 20, 100, 80));
    column.hide();
    CHECK(windowUnderPointer(screen(), {&column}) == QRect(300, 300, 100, 80));
}

TEST_CASE("no window, no monitor or nothing on screen is no window", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    desktop.win32.monitor = NativeRect{origin().x(), origin().y(), 0, 0};
    desktop.lister.underCursor = 0;
    CHECK_FALSE(windowUnderPointer(screen()).has_value());

    desktop.lister.underCursor = 42;
    desktop.lister.frames[42] = native(-500, -500, 100, 100);
    CHECK_FALSE(windowUnderPointer(screen()).has_value());

    desktop.lister.frames.erase(42);
    CHECK_FALSE(windowUnderPointer(screen()).has_value());

    desktop.lister.frames[42] = native(10, 10, 100, 100);
    desktop.win32.monitorThrows = true;
    CHECK_FALSE(windowUnderPointer(screen()).has_value());
}
