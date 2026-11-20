#pragma once

#include <vector>

#include <QSize>
#include <QWidget>

class QBoxLayout;

namespace fv {

// Controls on one line while they fit, else one under another (settings_styles.py _FitRow,
// object name "appStylesAddRow"): the Apps & styles pickers with their Add buttons. The first
// control takes the spare width and needs at least 180 px at the font scale, room for an app's
// name rather than only its arrow; below the sum of the minimum widths and the gaps the row
// stacks, the last control then aligned left.
// Affinity: GUI thread.
class FitRow : public QWidget {
    Q_OBJECT
public:
    // The first control's floor at 100 % text.
    static constexpr int kFirstMinWidth = 180;

    explicit FitRow(std::vector<QWidget*> widgets, QWidget* parent = nullptr);

    QBoxLayout* line() const { return line_; }
    bool isStacked() const;

    // The widest control's minimum: the row can always stack down to it.
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    // The width the controls need side by side.
    int needed() const;

    std::vector<QWidget*> widgets_;
    QBoxLayout* line_;
};

} // namespace fv
