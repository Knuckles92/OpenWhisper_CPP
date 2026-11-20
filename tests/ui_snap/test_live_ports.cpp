// The walkthrough runtime's Snap and live drawing ports over fv_ui_snap: every call forwarded and
// every outcome and signal reported, with screens as ScreenRef and marks as MarksPtr. Snaps go
// through the fake grabber of ScopedFakeDesktop; the live drawing is a LiveAnnotations offscreen.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <QApplication>
#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QPointer>
#include <QScreen>
#include <QTest>
#include <QWidget>

#include "base/testing/manual_executor.h"
#include "settings/testing/memory_store.h"
#include "ui_snap/annotations.h"
#include "ui_snap/capture.h"
#include "ui_snap/live.h"
#include "ui_snap/live_drawing_port.h"
#include "ui_snap/overlay.h"
#include "ui_snap/qimage_shot.h"
#include "ui_snap/session.h"
#include "ui_snap/snap_port.h"
#include "ui_snap/support.h"
#include "ui_snap/testing/fake_desktop.h"
#include "walkthrough/testing/fake_shot_image.h"

#include "overlay_test_support.h"
#include "snap_test_support.h"

using namespace fv::ui_snap;
using namespace fvtest;
using fv::ui_snap::testing::ScopedFakeDesktop;
namespace wt = fv::walkthrough;

namespace {

// What a snap reported, in order.
struct Outcomes {
    std::vector<std::string> events;
    wt::ShotImagePtr image;
    std::string reason;

    wt::SnapPort::Kept kept() {
        return [this](wt::ShotImagePtr shot) {
            events.emplace_back("kept");
            image = std::move(shot);
        };
    }
    wt::SnapPort::Dropped dropped() {
        return [this] {
            events.emplace_back("dropped");
        };
    }
    wt::SnapPort::Failed failed() {
        return [this](std::string why) {
            events.emplace_back("failed");
            reason = std::move(why);
        };
    }
};

void endSessions() {
    for (SnapSession* session : SnapSession::liveSessions()) {
        if (session->isActive()) {
            session->drop();
        }
    }
    flushDeletes();
}

} // namespace

TEST_CASE("screens cross the ports by name and place", "[snap_port]") {
    QScreen* primary = QGuiApplication::primaryScreen();
    const wt::ScreenRef ref = screenRef(primary);
    CHECK(ref.name == primary->name().toStdString());
    CHECK(ref.index == QGuiApplication::screens().indexOf(primary));
    CHECK(screenFor(ref) == primary);
    CHECK(screenFor({ref.name, 7}) == primary);
    CHECK(screenFor({"no such screen", 0}) == nullptr);
    CHECK(screenRef(nullptr) == wt::ScreenRef{});
}

TEST_CASE("marks cross the ports as a vector of annotations", "[snap_port]") {
    CHECK(marksPtr({}) == nullptr);
    CHECK(marksOf(nullptr).empty());
    const std::vector<Annotation> marks{
        Annotation::box(QPointF(1, 2), QPointF(30, 40), coral()),
        Annotation::step(QPointF(50, 50), 1, coral()),
    };
    CHECK(marksOf(marksPtr(marks)) == marks);
}

TEST_CASE("the snap port reports this desktop's support and window mode", "[snap_port]") {
    ScopedFakeDesktop desktop;
    QtSnapPort port;
    CHECK(port.support() == std::pair<bool, std::string>{true, ""});
    CHECK(port.window_mode_supported());
    port.request_access(); // nothing to ask on Windows
    ScopedFakeDesktop wayland("linux", QStringLiteral("wayland"));
    CHECK_FALSE(port.support().first);
    CHECK(port.support().second == QString::fromUtf16(kWaylandReason).toStdString());
    CHECK_FALSE(port.window_mode_supported());
}

TEST_CASE("the snap port keeps the whole screen after start returns", "[snap_port]") {
    ScopedFakeDesktop desktop;
    QWidget overlay;
    overlay.resize(100, 40);
    overlay.show();
    int keepOutAsked = 0;
    int started = 0;
    QtSnapPort port(nullptr, {.keepOut =
                                  [&] {
                                      ++keepOutAsked;
                                      return QList<QPointer<QWidget>>{&overlay};
                                  },
                              .floatAbove = {},
                              .beforeStart =
                                  [&] {
                                      ++started;
                                  }});
    Outcomes outcomes;
    QScreen* screen = screenUnderCursor();
    const std::shared_ptr<wt::SnapHandle> handle =
        port.start({.hint = "", .take = wt::SnapTake::Screen, .screen = screenRef(screen)},
                   outcomes.kept(), outcomes.dropped(), outcomes.failed());
    CHECK(outcomes.events.empty()); // never from inside start
    CHECK(handle->active());
    CHECK(keepOutAsked == 1);
    CHECK(started == 1);
    REQUIRE(pumpUntil([&outcomes] { return !outcomes.events.empty(); }));
    CHECK(outcomes.events == std::vector<std::string>{"kept"});
    REQUIRE(outcomes.image != nullptr);
    CHECK(outcomes.image->width() == screen->geometry().width());
    CHECK(dynamic_cast<const QImageShot*>(outcomes.image.get()) != nullptr);
    CHECK_FALSE(handle->active());
    flushDeletes();
    // The session has gone; where the screenshot came from stays with the handle.
    const QRect geometry = screen->geometry();
    CHECK(handle->capture_rect() ==
          wt::Rect{geometry.x(), geometry.y(), geometry.width(), geometry.height()});
    handle->drop(); // after the outcome: nothing
    CHECK(outcomes.events.size() == 1);
    CHECK(overlay.isVisible());
}

TEST_CASE("the snap port reports a dropped overlay and a failed capture", "[snap_port]") {
    ScopedFakeDesktop desktop;
    QtSnapPort port;
    SECTION("dropped") {
        Outcomes outcomes;
        const auto handle = port.start({.hint = "Drag to select"}, outcomes.kept(),
                                       outcomes.dropped(), outcomes.failed());
        REQUIRE(pumpUntil([] {
            const QList<SnapSession*> live = SnapSession::liveSessions();
            return !live.isEmpty() && !live.front()->overlays().isEmpty();
        }));
        CHECK(handle->active());
        handle->drop();
        CHECK(outcomes.events == std::vector<std::string>{"dropped"});
        CHECK_FALSE(handle->active());
        CHECK_FALSE(handle->capture_rect().has_value());
    }
    SECTION("failed") {
        desktop.grabber.fail = true;
        Outcomes outcomes;
        const auto handle = port.start({.take = wt::SnapTake::Screen}, outcomes.kept(),
                                       outcomes.dropped(), outcomes.failed());
        REQUIRE(pumpUntil([&outcomes] { return !outcomes.events.empty(); }));
        CHECK(outcomes.events == std::vector<std::string>{"failed"});
        CHECK(outcomes.reason == QString::fromUtf16(kCaptureFailed).toStdString());
    }
    endSessions();
}

TEST_CASE("the snap port fails a snap of a monitor that has gone", "[snap_port]") {
    // session.py's _start: a screen no longer in screens() is "no screen", never the pointer's.
    ScopedFakeDesktop desktop;
    int grabs = 0;
    desktop.grabber.image = [&grabs](QScreen& target) -> std::optional<QImage> {
        ++grabs;
        QImage image(target.geometry().size(), QImage::Format_RGB32);
        image.fill(Qt::black);
        return image;
    };
    QtSnapPort port;
    Outcomes outcomes;
    const auto handle =
        port.start({.take = wt::SnapTake::Screen, .screen = wt::ScreenRef{"gone", 9}},
                   outcomes.kept(), outcomes.dropped(), outcomes.failed());
    CHECK(outcomes.events.empty()); // never from inside start
    CHECK_FALSE(handle->active());
    CHECK(SnapSession::liveSessions().isEmpty());
    REQUIRE(pumpUntil([&outcomes] { return !outcomes.events.empty(); }));
    CHECK(outcomes.events == std::vector<std::string>{"failed"});
    CHECK(outcomes.reason == QString::fromUtf16(kNoScreen).toStdString());
    CHECK(grabs == 0);
    handle->keep();
    handle->drop();
    QApplication::processEvents();
    CHECK(outcomes.events.size() == 1);
    CHECK_FALSE(handle->capture_rect().has_value());
}

TEST_CASE("the snap port opens a whole-screen drawing for Draw", "[snap_port]") {
    ScopedFakeDesktop desktop;
    QtSnapPort port;
    Outcomes outcomes;
    QScreen* screen = screenUnderCursor();
    const auto handle =
        port.start({.hint = "Draw on it", .take = wt::SnapTake::Draw, .screen = screenRef(screen)},
                   outcomes.kept(), outcomes.dropped(), outcomes.failed());
    REQUIRE(pumpUntil([] {
        const QList<SnapSession*> live = SnapSession::liveSessions();
        return !live.isEmpty() && !live.front()->overlays().isEmpty();
    }));
    const QList<SnapOverlay*> overlays = SnapSession::liveSessions().front()->overlays();
    REQUIRE(overlays.size() == 1);
    CHECK(overlays.front()->snapScreen() == screen);
    CHECK(overlays.front()->tool() == SnapTool::Pen);
    REQUIRE(overlays.front()->selection().has_value());
    CHECK(overlays.front()->selection()->size() == screen->geometry().size());
    CHECK(outcomes.events.empty());
    handle->keep();
    CHECK(outcomes.events == std::vector<std::string>{"kept"});
    const QRect geometry = screen->geometry();
    CHECK(handle->capture_rect() ==
          wt::Rect{geometry.x(), geometry.y(), geometry.width(), geometry.height()});
    endSessions();
}

TEST_CASE("the snap port renders marks onto a crop at device resolution", "[snap_port]") {
    QtSnapPort port;
    QImage frozen(400, 300, QImage::Format_RGB32);
    frozen.fill(QColor(QStringLiteral("#194754")));
    frozen.setDevicePixelRatio(2);
    const auto shot = std::make_shared<QImageShot>(frozen);
    const std::vector<Annotation> marks{
        Annotation::stroke({QPointF(20, 20), QPointF(60, 60)}, coral())};

    const wt::ShotImagePtr whole = port.render(shot, std::nullopt, nullptr);
    CHECK(whole->width() == 400);
    CHECK(whole->device_pixel_ratio() == 2);

    const wt::ShotImagePtr cropped = port.render(shot, wt::Rect{10, 10, 100, 80}, marksPtr(marks));
    CHECK(cropped->width() == 200);
    CHECK(cropped->height() == 160);
    const QImage& image = static_cast<const QImageShot&>(*cropped).image();
    CHECK(image.pixelColor(60, 60) != frozen.pixelColor(60, 60));

    const auto foreign = std::make_shared<wt::FakeShotImage>(10, 10, 0x123456);
    CHECK_THROWS_AS(port.render(foreign, std::nullopt, nullptr), std::invalid_argument);
}

TEST_CASE("the live drawing port forwards every call", "[live_drawing_port]") {
    ScopedFakeDesktop desktop;
    fv::MemorySettingsStore settings;
    QWidget panel;
    QtLiveDrawingPort port(&panel, {{"keep_marks", "F8"}, {"clear", "F9"}},
                           {.settings = &settings});
    LiveAnnotations* live = port.live();
    REQUIRE(live != nullptr);
    CHECK(live->toolbar()->buttonRect(QStringLiteral("keep_marks")).isValid());
    for (const ToolbarButton& button : live->toolbar()->buttons()) {
        if (button.kind == QStringLiteral("keep_marks")) {
            CHECK(button.tip == QStringLiteral("Keep marks (F8)"));
        }
    }

    CHECK_FALSE(port.armed());
    port.arm();
    CHECK(port.armed());
    CHECK(live->tool() == SnapTool::Pen);
    port.disarm();
    CHECK_FALSE(port.armed());

    port.set_busy(true);
    CHECK(live->busy());
    port.set_busy(false);

    port.set_capture_countdown(2.0, 3.0);
    CHECK(live->captureDelay()->remaining() == 2.0);
    CHECK(live->toolbar()->dropEnabled());
    port.set_capture_countdown(0);
    CHECK_FALSE(live->toolbar()->dropEnabled());

    port.handle_mark_shortcut("keep_marks");
    CHECK(live->keepMarks());

    port.set_enabled(false);
    CHECK_FALSE(live->enabled());
    port.set_enabled(true);
    CHECK(live->enabled());

    QPointer<LiveAnnotations> watched = live;
    port.close();
    CHECK(port.live() == nullptr);
    flushDeletes();
    CHECK(watched.isNull());
    // A closed port answers and does nothing.
    port.arm();
    CHECK_FALSE(port.armed());
    CHECK_FALSE(port.capture_region().has_value());
    CHECK(port.pinned_on(screenRef(QGuiApplication::primaryScreen())) == nullptr);
}

TEST_CASE("the live drawing port reports regions, pinned marks and pending captures",
          "[live_drawing_port]") {
    ScopedFakeDesktop desktop;
    QScreen* screen = screenUnderCursor();
    const QRect geometry = screen->geometry();
    desktop.win32.monitor =
        NativeRect{geometry.left(), geometry.top(), geometry.right() + 1, geometry.bottom() + 1};
    fv::MemorySettingsStore settings;
    auto* live = new LiveAnnotations(nullptr, nullptr, {}, {.settings = &settings});
    QtLiveDrawingPort port(live);

    CHECK_FALSE(port.capture_region().has_value());
    live->setRegion(screen, QRect(50, 80, 130, 80));
    const std::optional<wt::CaptureRegion> region = port.capture_region();
    REQUIRE(region.has_value());
    CHECK(region->screen == screenRef(screen));
    CHECK(region->rect == wt::Rect{50, 80, 130, 80});
    CHECK(region->global == wt::Rect{geometry.x() + 50, geometry.y() + 80, 130, 80});
    live->clearRegion();

    port.arm();
    LiveCanvas* canvas = live->canvas(screen);
    REQUIRE(canvas != nullptr);
    mouse(canvas, QEvent::MouseButtonPress, 50, 80);
    mouse(canvas, QEvent::MouseButtonRelease, 90, 110, Qt::NoButton, Qt::ControlModifier);
    mouse(canvas, QEvent::MouseButtonPress, 60, 80);
    mouse(canvas, QEvent::MouseButtonRelease, 100, 110, Qt::NoButton);
    CHECK(port.interacting() == false);
    CHECK(marksOf(port.pinned_on(screenRef(screen))).size() == 1);
    CHECK(marksOf(port.pinned_at({20, 20})).size() == 1);
    CHECK(port.pinned_on(std::nullopt) == nullptr);

    // A capture of another monitor keeps this one's temporary mark; one of this monitor
    // takes it.
    port.set_capture_pending(true);
    port.set_capture_pending(false, true, wt::ScreenRef{"gone", 9});
    CHECK(canvas->items().size() == 2);
    port.set_capture_pending(true);
    port.set_capture_pending(false, true, screenRef(screen));
    CHECK(canvas->items().size() == 1);
    port.set_capture_pending(true);
    port.set_capture_pending(false, false, std::nullopt);
    CHECK(canvas->expireTimer().isActive() == false);
}

TEST_CASE("the live drawing port re-emits every signal", "[live_drawing_port]") {
    ScopedFakeDesktop desktop;
    fv::ManualExecutor ui;
    fv::MemorySettingsStore settings;
    auto* live = new LiveAnnotations(nullptr, nullptr, {}, {.settings = &settings});
    QtLiveDrawingPort port(live);
    std::vector<std::string> heard;
    std::vector<fv::Subscription> subscriptions;
    subscriptions.push_back(port.arming.connect(ui, [&] { heard.emplace_back("arming"); }));
    subscriptions.push_back(
        port.capture_changed.connect(ui, [&] { heard.emplace_back("capture_changed"); }));
    wt::MarksPtr drawnMarks;
    wt::ScreenRef drawnOn;
    subscriptions.push_back(port.drawn.connect(ui, [&](wt::ScreenRef screen, wt::MarksPtr marks) {
        heard.emplace_back("drawn");
        drawnOn = std::move(screen);
        drawnMarks = std::move(marks);
    }));
    subscriptions.push_back(port.capture_delay_changed.connect(
        ui, [&](int seconds) { heard.emplace_back("delay " + std::to_string(seconds)); }));
    subscriptions.push_back(
        port.capture_cancelled.connect(ui, [&] { heard.emplace_back("capture_cancelled"); }));
    subscriptions.push_back(port.interaction_changed.connect(ui, [&](bool active) {
        heard.emplace_back(active ? "interaction on" : "interaction off");
    }));
    subscriptions.push_back(port.enabled_changed.connect(
        ui, [&](bool enabled) { heard.emplace_back(enabled ? "enabled" : "disabled"); }));

    QScreen* screen = screenUnderCursor();
    port.arm();
    LiveCanvas* canvas = live->canvas(screen);
    REQUIRE(canvas != nullptr);
    mouse(canvas, QEvent::MouseButtonPress, 50, 80);
    mouse(canvas, QEvent::MouseButtonRelease, 90, 110, Qt::NoButton);
    live->setRegion(screen, QRect(10, 10, 100, 100));
    Q_EMIT live->captureDelay()->changed(5);
    live->clear();
    ui.run_all();
    CHECK(heard == std::vector<std::string>{"arming", "interaction on", "interaction off", "drawn",
                                            "capture_changed", "interaction off", "delay 5",
                                            "capture_cancelled", "interaction off"});
    CHECK(drawnOn == screenRef(screen));
    REQUIRE(marksOf(drawnMarks).size() == 1);
    CHECK(marksOf(drawnMarks)[0].kind == AnnotationKind::Stroke);

    // Closing still tells the runtime the drawing went away (it unregisters the mark
    // shortcuts on that), then stops forwarding.
    heard.clear();
    port.close();
    ui.run_all();
    CHECK(heard == std::vector<std::string>{"disabled"});
    flushDeletes();
}
