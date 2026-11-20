// Grabbing a screen and keeping our windows out of it, over the fake grabber
// (tests/test_snap_session.py::TestCapture, and the plumbing of capture.py).
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <optional>

#include <QGuiApplication>
#include <QImage>
#include <QScreen>
#include <QWidget>

#include "golden/golden.h"
#include "ui_snap/affinity.h"
#include "ui_snap/capture.h"
#include "ui_snap/testing/fake_desktop.h"

using namespace fv::ui_snap;
using fv::ui_snap::testing::FakeScreenGrabber;
using fv::ui_snap::testing::ScopedFakeDesktop;

namespace {

std::unique_ptr<QWidget> shownWindow() {
    auto window = std::make_unique<QWidget>();
    window->resize(120, 40);
    window->show();
    return window;
}

// A window whose own hide() would end what it shows, as the recording overlay's does.
class SessionWindow : public QWidget {
public:
    void hide() { // shadows QWidget::hide, as WaveformOverlay's hide hides more than the window
        ended = true;
        QWidget::hide();
    }
    bool ended = false;
};

} // namespace

// from tests/test_snap_session.py::TestCapture::test_the_ratio_comes_from_the_captured_size
TEST_CASE("the ratio comes from the captured size", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    QScreen* screen = QGuiApplication::primaryScreen();
    desktop.grabber.image = [](QScreen& s) {
        return FakeScreenGrabber::screenImage(s, 2);
    };

    const std::optional<QImage> image = grab(screen);

    REQUIRE(image.has_value());
    CHECK(image->devicePixelRatio() == 2.0);
    CHECK(image->deviceIndependentSize().toSize() == screen->geometry().size());
}

// from tests/test_snap_session.py::TestCapture::test_unpaintable_formats_are_converted
TEST_CASE("unpaintable formats are converted", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    QScreen* screen = QGuiApplication::primaryScreen();
    const QImage indexed =
        FakeScreenGrabber::screenImage(*screen).convertToFormat(QImage::Format_Indexed8);
    desktop.grabber.image = [indexed](QScreen&) {
        return indexed;
    };

    const std::optional<QImage> image = grab(screen);

    REQUIRE(image.has_value());
    CHECK(image->format() == QImage::Format_ARGB32_Premultiplied);
}

// from
// tests/test_snap_session.py::TestCapture::test_hidden_windows_get_two_frames_or_hyprlands_fade
TEST_CASE("hidden windows get two frames or hyprlands fade", "[test_snap_session]") {
    CHECK(settleMs(144.0, false, false) == kMinSettleMs);
    CHECK(settleMs(30.0, false, false) == 67);
    CHECK(settleMs(144.0, true, false) == kHyprlandSettleMs);
}

TEST_CASE("a screen waits two of its frames", "[test_snap_session]") {
    QScreen* screen = QGuiApplication::primaryScreen();
    const double rate = screen->refreshRate();
    const int frames = rate >= 24 ? static_cast<int>(std::ceil(2000 / rate)) : kMinSettleMs;
    CHECK(settleMs(screen) == std::max(frames, kMinSettleMs));
    CHECK(settleMs(nullptr) == kMinSettleMs);
}

TEST_CASE("settle_ms matches OpenWhisper", "[golden][test_snap_session]") {
    fv::golden::for_each("ui_qt.snap.capture", "settle_ms", [](const fv::golden::Case& c) {
        const std::optional<double> rate = c.arg<std::optional<double>>(0);
        CHECK(settleMs(rate.value_or(0.0), c.arg<bool>(1), c.arg<bool>(2)) == c.result<int>());
    });
}

TEST_CASE("a capture that fails or returns nothing is no image", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    QScreen* screen = QGuiApplication::primaryScreen();

    desktop.grabber.fail = true;
    CHECK_FALSE(grab(screen).has_value());

    desktop.grabber.fail = false;
    desktop.grabber.image = [](QScreen&) {
        return std::optional<QImage>();
    };
    CHECK_FALSE(grab(screen).has_value());

    desktop.grabber.image = [](QScreen&) {
        return std::optional<QImage>(QImage());
    };
    CHECK_FALSE(grab(screen).has_value());
    CHECK(desktop.grabber.calls.size() == 3);
}

TEST_CASE("windows are hidden for the grab and shown again without activating",
          "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    const auto window = shownWindow();
    auto* child = new QWidget(window.get());
    const auto hidden = shownWindow();
    hidden->hide();

    // One window listed twice, through a child, is hidden once.
    const QList<QPointer<QWidget>> gone = hideWindows({window.get(), child, hidden.get()});

    REQUIRE(gone.size() == 1);
    CHECK(gone[0] == window.get());
    CHECK_FALSE(window->isVisible());
    CHECK_FALSE(hidden->isVisible());

    restoreWindows(gone);

    CHECK(window->isVisible());
    CHECK_FALSE(window->testAttribute(Qt::WA_ShowWithoutActivating));
    CHECK_FALSE(hidden->isVisible());
}

TEST_CASE("a window that shows without activating keeps that", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    const auto window = shownWindow();
    window->setAttribute(Qt::WA_ShowWithoutActivating, true);

    restoreWindows(hideWindows({window.get()}));

    CHECK(window->isVisible());
    CHECK(window->testAttribute(Qt::WA_ShowWithoutActivating));
}

TEST_CASE("windows kept out by affinity and minimised ones stay put", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    const auto kept = shownWindow();
    REQUIRE(keepOutOfScreenshots(kept.get(), true));
    const auto minimised = shownWindow();
    minimised->showMinimized();
    REQUIRE(minimised->isMinimized());

    CHECK(hideWindows({kept.get(), minimised.get()}).isEmpty());
    CHECK(kept->isVisible());
}

TEST_CASE("hiding for a capture is Qt's hide, not the window's own", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    SessionWindow overlay;
    overlay.show();

    const QList<QPointer<QWidget>> gone = hideWindows({&overlay});

    CHECK(gone.size() == 1);
    CHECK_FALSE(overlay.isVisible());
    CHECK_FALSE(overlay.ended);
}

TEST_CASE("deleted windows are skipped", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    auto window = shownWindow();
    QList<QPointer<QWidget>> listed{window.get(), nullptr};
    const QList<QPointer<QWidget>> gone = hideWindows(listed);
    REQUIRE(gone.size() == 1);
    window.reset();

    restoreWindows(gone); // the deleted window is passed over
    CHECK(gone[0].isNull());
}

TEST_CASE("a click finds its screen by the monitor's origin", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    QScreen* primary = QGuiApplication::primaryScreen();
    const QPoint origin = primary->geometry().topLeft();
    desktop.win32.monitor =
        NativeRect{origin.x(), origin.y(), origin.x() + 1920, origin.y() + 1080};

    CHECK(screenAtClick(QPoint(40, 30)) == primary);
    REQUIRE(desktop.win32.monitorQueries.size() == 1);
    CHECK(desktop.win32.monitorQueries[0] == std::pair{QPoint(40, 30), false});

    desktop.win32.monitor = NativeRect{origin.x() + 5000, 0, origin.x() + 6000, 1000};
    CHECK(screenAtWindowsPoint(QPoint(5100, 30)) == nullptr);

    desktop.win32.monitor.reset();
    CHECK(screenAtWindowsPoint(QPoint(40, 30)) == nullptr);

    desktop.win32.monitorThrows = true;
    CHECK(screenAtWindowsPoint(QPoint(40, 30)) == nullptr);
}

TEST_CASE("elsewhere a click finds its screen through Qt", "[test_snap_session]") {
    ScopedFakeDesktop desktop("linux", QStringLiteral("xcb"));
    QScreen* primary = QGuiApplication::primaryScreen();

    CHECK(screenAtClick(primary->geometry().center()) == primary);
    CHECK(desktop.win32.monitorQueries.empty());
}

TEST_CASE("the pointer's screen is covered first", "[test_snap_session]") {
    ScopedFakeDesktop desktop;
    const QList<QScreen*> screens = screensToCover();
    REQUIRE(screens.size() == QGuiApplication::screens().size());
    CHECK(screens.front() == screenUnderCursor());
    CHECK(screenUnderCursor() != nullptr);
}
