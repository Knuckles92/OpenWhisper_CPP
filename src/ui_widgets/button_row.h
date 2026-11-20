#pragma once

#include <optional>
#include <utility>
#include <vector>

#include <QWidget>

class QPushButton;
class QTimer;

namespace fv {

// Buttons share one row while their labels fit, then wrap into more rows.
//
// A plain QHBoxLayout lets a stretchy button shrink below its label when the column is narrow
// (a 300 px inspector at 130% font scale), so its neighbours paint over it. Here every button
// keeps its own label width as a floor and the row count grows until each row fits
// (button_row_sizes). The short row goes first, so three links wrap as 1 + 2 with the main link
// on top, and each row's buttons share its full width. With stretch off the buttons keep their
// own width and sit at the left, like chips, and still wrap onto more rows when narrow.
//
// The buttons are reparented here at construction. The width comes from the enclosing column
// (size policy Ignored x Preferred). Affinity: GUI thread.
class ButtonRow : public QWidget {
    Q_OBJECT
public:
    explicit ButtonRow(std::vector<QPushButton*> buttons, bool stretch = true,
                       QWidget* parent = nullptr, int spacing = 8);

    // Re-picks the rows; call after changing a label or showing or hiding a button. A label,
    // font, style or visibility change on a child also refreshes, on the next event-loop pass.
    void refresh();

    const std::vector<QPushButton*>& buttons() const { return buttons_; }

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

private:
    std::vector<QPushButton*> buttons_;
    int spacing_;
    bool stretch_;
    // The rows and the visible buttons they were laid out for.
    std::optional<std::pair<std::vector<int>, std::vector<QPushButton*>>> shape_;
    QTimer* reflowTimer_ = nullptr;
};

} // namespace fv
