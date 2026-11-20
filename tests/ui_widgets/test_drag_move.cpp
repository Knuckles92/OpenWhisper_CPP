// DragToMove: carrying a frameless window with the pointer, kept on the screen under it.
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QMouseEvent>
#include <QScreen>
#include <QWidget>

#include "ui_widgets/drag_move.h"

namespace {

QMouseEvent mouse(QEvent::Type type, const QPoint& global, Qt::MouseButton button) {
    return QMouseEvent(type, QPointF(global), QPointF(global), button,
                       type == QEvent::MouseButtonRelease ? Qt::NoButton : button, Qt::NoModifier);
}

struct Floating {
    Floating() {
        window.setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
        window.resize(200, 100);
        window.move(100, 100);
        window.show();
        QApplication::processEvents();
    }
    QWidget window;
};

} // namespace

TEST_CASE("DragToMove gives the window the open hand", "[widget]") {
    Floating floating;
    fv::DragToMove drag(&floating.window);
    CHECK(floating.window.cursor().shape() == Qt::OpenHandCursor);
    CHECK_FALSE(drag.moved());
}

TEST_CASE("DragToMove carries the window with the pointer", "[widget]") {
    Floating floating;
    fv::DragToMove drag(&floating.window);
    const QPoint start = floating.window.pos();

    QMouseEvent press = mouse(QEvent::MouseButtonPress, start + QPoint(50, 20), Qt::LeftButton);
    CHECK(drag.press(&press));
    CHECK(press.isAccepted());
    CHECK(floating.window.cursor().shape() == Qt::ClosedHandCursor);

    QMouseEvent move = mouse(QEvent::MouseMove, start + QPoint(80, 45), Qt::LeftButton);
    CHECK(drag.move(&move));
    CHECK(floating.window.pos() == start + QPoint(30, 25));
    CHECK(drag.moved());

    QMouseEvent release = mouse(QEvent::MouseButtonRelease, start + QPoint(80, 45), Qt::LeftButton);
    CHECK(drag.release(&release));
    CHECK(floating.window.cursor().shape() == Qt::OpenHandCursor);
    // Released: further moves are not the drag's.
    QMouseEvent after = mouse(QEvent::MouseMove, start + QPoint(120, 90), Qt::NoButton);
    CHECK_FALSE(drag.move(&after));
    CHECK(floating.window.pos() == start + QPoint(30, 25));
}

TEST_CASE("DragToMove ignores other buttons and a move or release with nothing held", "[widget]") {
    Floating floating;
    fv::DragToMove drag(&floating.window);
    QMouseEvent right = mouse(QEvent::MouseButtonPress, QPoint(150, 120), Qt::RightButton);
    CHECK_FALSE(drag.press(&right));
    QMouseEvent move = mouse(QEvent::MouseMove, QPoint(300, 300), Qt::NoButton);
    CHECK_FALSE(drag.move(&move));
    QMouseEvent release = mouse(QEvent::MouseButtonRelease, QPoint(150, 120), Qt::LeftButton);
    CHECK_FALSE(drag.release(&release));
    CHECK_FALSE(drag.moved());
}

TEST_CASE("DragToMove clamps to the screen under the pointer with margin 8", "[widget]") {
    Floating floating;
    fv::DragToMove drag(&floating.window);
    const QRect available = floating.window.screen()->availableGeometry();
    QMouseEvent press = mouse(QEvent::MouseButtonPress, QPoint(150, 120), Qt::LeftButton);
    REQUIRE(drag.press(&press));

    // Dragged far past the bottom right, the window stops 8 px inside both edges.
    QMouseEvent far =
        mouse(QEvent::MouseMove, available.bottomRight() + QPoint(900, 900), Qt::LeftButton);
    drag.move(&far);
    CHECK(floating.window.x() + floating.window.width() == available.x() + available.width() - 8);
    CHECK(floating.window.y() + floating.window.height() == available.y() + available.height() - 8);

    // And past the top left, 8 px inside those.
    QMouseEvent near =
        mouse(QEvent::MouseMove, available.topLeft() - QPoint(900, 900), Qt::LeftButton);
    drag.move(&near);
    CHECK(floating.window.pos() == available.topLeft() + QPoint(8, 8));
}

TEST_CASE("DragToMove remembers a drag until reset", "[widget]") {
    Floating floating;
    fv::DragToMove drag(&floating.window);
    QMouseEvent press = mouse(QEvent::MouseButtonPress, QPoint(150, 120), Qt::LeftButton);
    drag.press(&press);
    QMouseEvent move = mouse(QEvent::MouseMove, QPoint(170, 140), Qt::LeftButton);
    drag.move(&move);
    REQUIRE(drag.moved());

    drag.reset();
    CHECK_FALSE(drag.moved());
    // The grab is gone too: a move now is not the drag's.
    QMouseEvent again = mouse(QEvent::MouseMove, QPoint(190, 160), Qt::LeftButton);
    CHECK_FALSE(drag.move(&again));
}

TEST_CASE("a move that leaves the window where it is does not count as moved", "[widget]") {
    Floating floating;
    fv::DragToMove drag(&floating.window);
    QMouseEvent press = mouse(QEvent::MouseButtonPress, QPoint(150, 120), Qt::LeftButton);
    drag.press(&press);
    QMouseEvent same = mouse(QEvent::MouseMove, QPoint(150, 120), Qt::LeftButton);
    CHECK(drag.move(&same));
    CHECK_FALSE(drag.moved());
}
