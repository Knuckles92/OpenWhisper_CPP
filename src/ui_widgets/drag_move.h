#pragma once

#include <optional>

#include <QPoint>

class QMouseEvent;
class QWidget;

namespace fv {

// Carries a frameless floating window with the pointer: moves the window while the left button
// holds it, kept (margin 8) on the screen under the pointer. The window forwards its mouse
// events here; each handler returns true when the event was the drag's. moved() stays set until
// reset(), so a window that places itself can leave a spot the user chose alone.
// Affinity: GUI thread.
class DragToMove {
public:
    // Gives the window the open-hand cursor.
    explicit DragToMove(QWidget* window);

    bool press(QMouseEvent* event);
    bool move(QMouseEvent* event);
    bool release(QMouseEvent* event);

    // Forgets the drag: the window's next placement is its own again.
    void reset();
    bool moved() const { return moved_; }

private:
    QWidget* window_;
    std::optional<QPoint> grab_;
    bool moved_ = false;
};

} // namespace fv
