// A removed screen's canvas does not outlive its LiveAnnotations: deleting the drawing directly
// (not through close()) before the event loop runs deletes the retired canvas with it, so the
// canvas never reads a dangling owner.

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QGuiApplication>
#include <QPointer>
#include <QRect>
#include <QScreen>

#include "base/testing/fake_clock.h"
#include "settings/testing/memory_store.h"
#include "ui_snap/live.h"
#include "ui_snap/testing/fake_desktop.h"

#include "overlay_test_support.h"
#include "two_screens.h"

using namespace fv::ui_snap;

TEST_CASE("deleting live drawing directly deletes a removed screen's canvas with it", "[widget]") {
    fvtest::ScopedScreens screens({QRect(0, 0, 800, 600), QRect(800, 0, 640, 480)});
    if (!screens.available()) {
        SKIP("the offscreen platform takes no second screen here");
    }
    testing::ScopedFakeDesktop desktop;
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    auto* drawing =
        new LiveAnnotations(nullptr, nullptr, {}, {.settings = &settings, .clock = &clock});
    QScreen* second = QGuiApplication::screens().at(1);
    drawing->arm(SnapTool::Pen);
    QPointer<LiveCanvas> retired = drawing->canvas(second);
    REQUIRE(retired != nullptr);
    {
        fvtest::ScopedScreens one({QRect(0, 0, 800, 600)});
        // Retired, its delete only queued: still alive here.
        REQUIRE(retired != nullptr);
        delete drawing;
        CHECK(retired == nullptr);
        fvtest::flushDeletes();
    }
}

// The same with one screen, for a platform that takes no second one: the screen's removal is
// announced as Qt announces it (screenRemoved), while the screen object stays.
TEST_CASE("a canvas retired by screenRemoved goes with its drawing when that is deleted directly",
          "[widget]") {
    testing::ScopedFakeDesktop desktop;
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    auto* drawing =
        new LiveAnnotations(nullptr, nullptr, {}, {.settings = &settings, .clock = &clock});
    QScreen* screen = QGuiApplication::primaryScreen();
    drawing->arm(SnapTool::Pen);
    QPointer<LiveCanvas> retired = drawing->canvas(screen);
    REQUIRE(retired != nullptr);
    Q_EMIT qGuiApp->screenRemoved(screen);
    CHECK(drawing->canvas(screen) == nullptr);
    REQUIRE(retired != nullptr);
    delete drawing;
    CHECK(retired == nullptr);
    fvtest::flushDeletes();
}
