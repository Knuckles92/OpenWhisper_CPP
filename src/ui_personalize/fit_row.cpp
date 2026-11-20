#include "ui_personalize/fit_row.h"

#include <algorithm>
#include <utility>

#include <QBoxLayout>

#include "pyport/number.h"
#include "ui_theme/font_scale.h"

namespace fv {

FitRow::FitRow(std::vector<QWidget*> widgets, QWidget* parent)
    : QWidget(parent), widgets_(std::move(widgets)) {
    setObjectName(QStringLiteral("appStylesAddRow"));
    line_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
    line_->setContentsMargins(0, 0, 0, 0);
    line_->setSpacing(8);
    for (std::size_t index = 0; index < widgets_.size(); ++index) {
        line_->addWidget(widgets_[index], index == 0 ? 1 : 0);
    }
}

bool FitRow::isStacked() const {
    return line_->direction() == QBoxLayout::TopToBottom;
}

int FitRow::needed() const {
    int total = 0;
    for (std::size_t index = 0; index < widgets_.size(); ++index) {
        int width = widgets_[index]->minimumSizeHint().width();
        if (index == 0) {
            const double scaled = kFirstMinWidth * ui::currentUiFontScale();
            width = std::max(width, static_cast<int>(py::round_half_even(scaled)));
        }
        total += width;
    }
    const int gaps = widgets_.empty() ? 0 : static_cast<int>(widgets_.size()) - 1;
    return total + line_->spacing() * gaps;
}

void FitRow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (widgets_.empty()) {
        return;
    }
    const bool stacked = width() < needed();
    const QBoxLayout::Direction direction =
        stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    if (line_->direction() != direction) {
        line_->setDirection(direction);
        line_->setAlignment(widgets_.back(), stacked ? Qt::AlignLeft : Qt::AlignVCenter);
    }
}

QSize FitRow::minimumSizeHint() const {
    QSize hint = QWidget::minimumSizeHint();
    int widest = 0;
    for (const QWidget* widget : widgets_) {
        widest = std::max(widest, widget->minimumSizeHint().width());
    }
    hint.setWidth(widest);
    return hint;
}

} // namespace fv
