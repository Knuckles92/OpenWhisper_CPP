// The splash: drag to move, its window, its texts and its glow clock. Ported from
// tests/test_loading_screen.py.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>

#include <QApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QScreen>
#include <QTimer>

#include "ui_shell/loading_screen.h"

using fv::LoadingScreen;

namespace {

void send(QWidget& widget, QEvent::Type type, QPoint global, Qt::MouseButton button,
          Qt::MouseButtons buttons) {
    const QPointF local = widget.mapFromGlobal(QPointF(global));
    QMouseEvent event(type, local, QPointF(global), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

} // namespace

// from tests/test_loading_screen.py::test_loading_screen_drags_with_left_mouse
TEST_CASE("loading screen drags with left mouse", "[test_loading_screen][widget]") {
    LoadingScreen screen;
    const QPoint origin = screen.pos();

    send(screen, QEvent::MouseButtonPress, origin + QPoint(20, 20), Qt::LeftButton, Qt::LeftButton);
    CHECK(screen.dragPosition().has_value());

    send(screen, QEvent::MouseMove, origin + QPoint(60, 50), Qt::NoButton, Qt::LeftButton);
    CHECK(screen.pos() == origin + QPoint(40, 30));

    send(screen, QEvent::MouseButtonRelease, origin + QPoint(60, 50), Qt::LeftButton, Qt::NoButton);
    CHECK_FALSE(screen.dragPosition().has_value());
}

TEST_CASE("the splash is a fixed frameless tool window centred on the primary screen", "[widget]") {
    LoadingScreen screen;
    CHECK(screen.size() == QSize(450, 300));
    CHECK(screen.minimumSize() == screen.maximumSize());
    const Qt::WindowFlags flags = screen.windowFlags();
    CHECK((flags & Qt::FramelessWindowHint));
    CHECK((flags & Qt::WindowStaysOnTopHint));
    CHECK((flags & Qt::Tool) == Qt::Tool);
    CHECK(screen.testAttribute(Qt::WA_TranslucentBackground));
    const QRect area = QGuiApplication::primaryScreen()->geometry();
    CHECK(screen.pos() == QPoint(area.center().x() - 225, area.center().y() - 150));
    CHECK(screen.statusText() == QStringLiteral("Initializing..."));
    CHECK(screen.progressText() == QStringLiteral("Please wait..."));

    screen.updateStatus(QStringLiteral("Initializing components..."));
    screen.updateProgress(QStringLiteral("Preparing startup..."));
    CHECK(screen.statusText() == QStringLiteral("Initializing components..."));
    CHECK(screen.progressText() == QStringLiteral("Preparing startup..."));
}

TEST_CASE("the glow follows the wall clock at 4.5 radians a second unless pinned", "[widget]") {
    LoadingScreen screen;
    auto* timer = screen.findChild<QTimer*>();
    REQUIRE(timer != nullptr);
    CHECK(timer->isActive());
    CHECK(timer->interval() == 33);
    CHECK(timer->timerType() == Qt::PreciseTimer);

    const double before = screen.glowPhase();
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 120) {
        QApplication::processEvents();
    }
    const double after = screen.glowPhase();
    CHECK(after - before >= 0.1 * 4.5);

    screen.pinGlowPhase(0.0);
    CHECK(screen.glowPhase() == 0.0);
    screen.pinGlowPhase(std::nullopt);
    CHECK(screen.glowPhase() > 0.0);

    screen.stopAnimation();
    CHECK_FALSE(timer->isActive());
}

TEST_CASE("closing the splash stops its glow", "[widget]") {
    LoadingScreen screen;
    screen.show();
    QApplication::processEvents();
    screen.close();
    CHECK_FALSE(screen.findChild<QTimer*>()->isActive());
}
