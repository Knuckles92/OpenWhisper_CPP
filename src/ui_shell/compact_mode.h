#pragma once

#include <optional>

#include <QObject>
#include <QPointer>
#include <QRect>

class QVBoxLayout;
class QWidget;

namespace fv {

class MainWindow;

// Compact Mode (main_window.py:1496-1625): the window swaps its tabs for the compact recording
// controller at a fixed 420 x 250, hides the Stats button, the side panel and its edge tab, the
// title and the maximize button, and keeps its own position (compact_window_geometry, x and y
// only). Leaving it brings back the full window's geometry from before. The choice is saved in
// compact_mode; a failed save logs a warning and keeps the mode.
// Affinity: UI actor (owned by MainWindow).
class CompactMode : public QObject {
    Q_OBJECT
public:
    explicit CompactMode(MainWindow& window);

    // Adds the controller, hidden, below the tabs in layout; a later controller replaces it.
    void setController(QWidget* controller, QVBoxLayout* layout);
    QWidget* controller() const { return controller_; }

    bool isOn() const { return on_; }
    // set_compact_mode, without the signal.
    void set(bool compact, bool persist);
    // _restore_compact_mode.
    void restore();
    // _save_compact_geometry.
    void saveGeometry();
    // _restore_compact_geometry.
    void restoreGeometry();

private:
    void enter();
    void leave();

    MainWindow& window_;
    QPointer<QWidget> controller_;
    bool on_ = false;
    std::optional<QRect> fullGeometry_;
};

} // namespace fv
