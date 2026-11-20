#pragma once

#include <utility>
#include <vector>

#include <QFrame>

class QBoxLayout;
class QHBoxLayout;
class QVBoxLayout;

namespace fv {

// A Hotkeys page card with text on the left and controls on the right, or the controls under
// the text when the card is too narrow for both (ui_qt/widgets/hotkey_row.py). Fill
// copyLayout() and addControl(); addFooter() adds a full-width row underneath. Stacked, the
// first control also gives up width before the row would overflow.
// Affinity: GUI thread.
class ReflowCard : public QFrame {
    Q_OBJECT

public:
    // The narrowest the text column may get beside the controls, at 100 % font scale, in px.
    static constexpr int kMinCopyWidth = 200;

    explicit ReflowCard(QWidget* parent = nullptr);

    QVBoxLayout* copyLayout() const { return copy_; }
    QHBoxLayout* controlsLayout() const { return controls_; }
    void addControl(QWidget* widget);
    void addFooter(QWidget* widget);
    bool isStacked() const { return stacked_; }

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void reflow(int width);

    QVBoxLayout* outer_;
    QBoxLayout* box_;
    QVBoxLayout* copy_;
    QHBoxLayout* controls_;
    // Each control with the minimum width it was added with.
    std::vector<std::pair<QWidget*, int>> controlWidgets_;
    bool stacked_ = false;
};

} // namespace fv
