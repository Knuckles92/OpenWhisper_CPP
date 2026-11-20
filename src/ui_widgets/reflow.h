#pragma once

#include <optional>

#include <QWidget>

class QGridLayout;

namespace fv {

// A lead widget with actions beside it, or below it once the row is narrow (object name
// "reflow").
//
// The width comes from the column the row sits in (its own minimum is ignored), so a narrow
// tiled window or a large font moves the actions to their own line instead of pushing them past
// the edge, or holding the whole page wider than its window. The actions stack when the width is
// below lead + 8 + the actions' natural width, where lead is leadWidth pixels at 100% font
// scale, or, without leadWidth, the lead's own natural width in the current font (a lead that
// cannot shrink: a button or a check box). Affinity: GUI thread.
class Reflow : public QWidget {
    Q_OBJECT
public:
    Reflow(QWidget* lead, QWidget* actions, std::optional<int> leadWidth = std::nullopt,
           QWidget* parent = nullptr, Qt::Alignment stackedAlignment = Qt::AlignRight);

    bool isStacked() const { return stacked_.value_or(false); }

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void place(bool stacked);
    void reflow();

    QWidget* lead_;
    QWidget* actions_;
    std::optional<int> leadWidth_;
    Qt::Alignment stackedAlignment_;
    std::optional<bool> stacked_;
    QGridLayout* grid_;
};

} // namespace fv
