#include "ui_history/chip_flow.h"

#include <algorithm>
#include <numeric>

#include <QEvent>
#include <QSizePolicy>

namespace fv {

ChipFlow::ChipFlow(std::vector<QWidget*> chips, QWidget* parent)
    : QWidget(parent), chips_(std::move(chips)) {
    for (QWidget* chip : chips_) {
        chip->setParent(this);
    }
    QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}

bool ChipFlow::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::LayoutRequest:
    case QEvent::Show:
    case QEvent::FontChange:
    case QEvent::StyleChange:
        if (measured_) {
            const Measure previous = *measured_;
            measured_.reset();
            // Only a real change may invalidate the layouts above: Qt sends these often while
            // a long list is built.
            if (measure() != previous) {
                heights_.clear();
                updateGeometry();
                arrange(width(), true);
            }
        }
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

const ChipFlow::Measure& ChipFlow::measure() const {
    if (!measured_) {
        Measure made;
        for (QWidget* chip : chips_) {
            // The stylesheet sets the font their width is measured in.
            chip->ensurePolished();
        }
        for (QWidget* chip : chips_) {
            const QSize hint = chip->sizeHint();
            made.widths.push_back(hint.width());
            made.line = std::max(made.line, hint.height());
        }
        measured_ = std::move(made);
    }
    return *measured_;
}

int ChipFlow::arrange(int width, bool apply) const {
    const Measure& sizes = measure();
    int x = 0;
    int y = 0;
    for (std::size_t i = 0; i < chips_.size(); ++i) {
        const int chipWidth = std::min(sizes.widths[i], std::max(1, width));
        if (x != 0 && x + chipWidth > width) {
            x = 0;
            y += sizes.line + kSpacing;
        }
        if (apply) {
            chips_[i]->setGeometry(x, y, chipWidth, sizes.line);
        }
        x += chipWidth + kSpacing;
    }
    return y + sizes.line;
}

int ChipFlow::heightForWidth(int width) const {
    const auto found = heights_.find(width);
    if (found != heights_.end()) {
        return found->second;
    }
    if (heights_.size() >= kHeightCacheSize) {
        heights_.clear();
    }
    const int height = arrange(width, false);
    heights_[width] = height;
    return height;
}

QSize ChipFlow::sizeHint() const {
    const Measure& sizes = measure();
    const int gaps = kSpacing * std::max(0, static_cast<int>(sizes.widths.size()) - 1);
    const int natural = std::accumulate(sizes.widths.begin(), sizes.widths.end(), 0) + gaps;
    return {natural, heightForWidth(width() != 0 ? width() : natural)};
}

QSize ChipFlow::minimumSizeHint() const {
    return {0, measure().line};
}

void ChipFlow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    arrange(width(), true);
}

} // namespace fv
