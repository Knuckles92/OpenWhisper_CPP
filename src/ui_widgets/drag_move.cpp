#include "ui_widgets/drag_move.h"

#include <QApplication>
#include <QMouseEvent>
#include <QScreen>
#include <QWidget>

#include "uimath/overlay_position.h"

namespace fv {

DragToMove::DragToMove(QWidget* window) : window_(window) {
    window->setCursor(Qt::OpenHandCursor);
}

bool DragToMove::press(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return false;
    }
    grab_ = event->globalPosition().toPoint() - window_->pos();
    window_->setCursor(Qt::ClosedHandCursor);
    event->accept();
    return true;
}

bool DragToMove::move(QMouseEvent* event) {
    if (!grab_) {
        return false;
    }
    const QPoint pointer = event->globalPosition().toPoint();
    const QPoint target = pointer - *grab_;
    if (target != window_->pos()) {
        int x = target.x();
        int y = target.y();
        QScreen* screen = QApplication::screenAt(pointer);
        if (screen == nullptr) {
            screen = window_->screen();
        }
        if (screen != nullptr) {
            const QSize size = window_->size();
            const QRect available = screen->availableGeometry();
            const Point clamped = clamp_rect_to_available(
                x, y, size.width(), size.height(),
                Rect{available.x(), available.y(), available.width(), available.height()});
            x = clamped.x;
            y = clamped.y;
        }
        window_->move(x, y);
        moved_ = true;
    }
    event->accept();
    return true;
}

bool DragToMove::release(QMouseEvent* event) {
    if (!grab_ || event->button() != Qt::LeftButton) {
        return false;
    }
    grab_.reset();
    window_->setCursor(Qt::OpenHandCursor);
    event->accept();
    return true;
}

void DragToMove::reset() {
    grab_.reset();
    moved_ = false;
}

} // namespace fv
