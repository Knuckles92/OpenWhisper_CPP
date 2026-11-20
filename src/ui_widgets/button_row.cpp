#include "ui_widgets/button_row.h"

#include <algorithm>
#include <numeric>

#include <QEvent>
#include <QGridLayout>
#include <QPushButton>
#include <QSizePolicy>
#include <QTimer>

#include "ui_widgets/layout_math.h"

namespace fv {

ButtonRow::ButtonRow(std::vector<QPushButton*> buttons, bool stretch, QWidget* parent, int spacing)
    : QWidget(parent), buttons_(std::move(buttons)), spacing_(spacing), stretch_(stretch) {
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* layout = new QGridLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(spacing);
    reflowTimer_ = new QTimer(this);
    reflowTimer_->setSingleShot(true);
    connect(reflowTimer_, &QTimer::timeout, this, &ButtonRow::refresh);
    // Adopted now: a parentless widget reports itself hidden until it is shown, and showing one
    // before it is placed would open it as its own window.
    for (QPushButton* button : buttons_) {
        button->setParent(this);
    }
    refresh();
}

void ButtonRow::refresh() {
    std::vector<QPushButton*> visible;
    for (QPushButton* button : buttons_) {
        if (!button->isHidden()) {
            visible.push_back(button);
        }
    }
    std::vector<int> widths;
    int widest = 0;
    for (QPushButton* button : visible) {
        // Button::setText sets its own floor; compact buttons clear it, so raise it.
        button->setMinimumWidth(std::max(button->minimumWidth(), button->sizeHint().width()));
        widths.push_back(button->minimumWidth());
        widest = std::max(widest, button->minimumWidth());
    }
    // Even a row of one needs the widest label, so a column that would squeeze it narrower (a
    // two-column panel) stacks instead.
    setMinimumWidth(widest);
    std::vector<int> sizes = button_row_sizes(widths, width(), spacing_);
    if (shape_ && shape_->first == sizes && shape_->second == visible) {
        return;
    }
    shape_.emplace(sizes, visible);
    auto* grid = static_cast<QGridLayout*>(layout());
    for (QPushButton* button : buttons_) {
        grid->removeWidget(button);
    }
    std::size_t placed = 0;
    if (!stretch_) {
        // Chips: natural widths from the left, the spare width after them.
        for (int row = 0; row < static_cast<int>(sizes.size()); ++row) {
            for (int slot = 0; slot < sizes[static_cast<std::size_t>(row)]; ++slot) {
                grid->addWidget(visible[placed++], row, slot, Qt::AlignLeft);
            }
        }
        const int columns = sizes.empty() ? 0 : *std::max_element(sizes.begin(), sizes.end());
        for (int column = 0; column < std::max(columns + 1, grid->columnCount()); ++column) {
            grid->setColumnStretch(column, column == columns ? 1 : 0);
        }
        updateGeometry();
        return;
    }
    // Every row spans the same grid, so a row of one is as wide as a row of three.
    const int columns =
        std::accumulate(sizes.begin(), sizes.end(), 1, [](int a, int b) { return std::lcm(a, b); });
    for (int row = 0; row < static_cast<int>(sizes.size()); ++row) {
        const int size = sizes[static_cast<std::size_t>(row)];
        const int span = columns / size;
        for (int slot = 0; slot < size; ++slot) {
            grid->addWidget(visible[placed++], row, slot * span, 1, span);
        }
    }
    for (int column = 0; column < std::max(columns, grid->columnCount()); ++column) {
        grid->setColumnStretch(column, column < columns ? 1 : 0);
    }
    updateGeometry();
}

void ButtonRow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    refresh();
}

bool ButtonRow::event(QEvent* event) {
    // A label or visibility change on a child is a layout request for this widget.
    const QEvent::Type type = event->type();
    if ((type == QEvent::LayoutRequest || type == QEvent::FontChange ||
         type == QEvent::StyleChange) &&
        reflowTimer_ != nullptr) {
        reflowTimer_->start(0);
    }
    return QWidget::event(event);
}

} // namespace fv
