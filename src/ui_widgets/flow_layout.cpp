#include "ui_widgets/flow_layout.h"

#include <algorithm>
#include <vector>

#include "ui_widgets/layout_math.h"

namespace fv {

FlowLayout::FlowLayout(QWidget* parent, int spacing) : QLayout(parent), gap_(spacing) {
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout() {
    // The items are the layout's; the widgets they hold stay with their parent widget.
    while (!items_.isEmpty()) {
        delete items_.takeFirst();
    }
}

void FlowLayout::addItem(QLayoutItem* item) {
    items_.append(item);
}

int FlowLayout::count() const {
    return static_cast<int>(items_.size());
}

QLayoutItem* FlowLayout::itemAt(int index) const {
    return index >= 0 && index < items_.size() ? items_.at(index) : nullptr;
}

QLayoutItem* FlowLayout::takeAt(int index) {
    return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const {
    return {};
}

bool FlowLayout::hasHeightForWidth() const {
    return true;
}

int FlowLayout::heightForWidth(int width) const {
    return arrange(QRect(0, 0, width, 0), false);
}

void FlowLayout::setGeometry(const QRect& rect) {
    QLayout::setGeometry(rect);
    arrange(rect, true);
}

QSize FlowLayout::sizeHint() const {
    return minimumSize();
}

QSize FlowLayout::minimumSize() const {
    QSize size;
    for (const QLayoutItem* item : items_) {
        if (!item->isEmpty()) {
            size = size.expandedTo(item->minimumSize());
        }
    }
    return size;
}

int FlowLayout::lineWidth() const {
    int total = 0;
    int shown = 0;
    for (const QLayoutItem* item : items_) {
        if (!item->isEmpty()) {
            total += item->sizeHint().width();
            ++shown;
        }
    }
    return total + gap_ * std::max(0, shown - 1);
}

int FlowLayout::arrange(const QRect& rect, bool move) const {
    std::vector<QLayoutItem*> placed;
    std::vector<Size> sizes;
    for (QLayoutItem* item : items_) {
        if (item->isEmpty()) {
            continue;
        }
        const QSize hint = item->sizeHint();
        placed.push_back(item);
        sizes.push_back({hint.width(), hint.height()});
    }
    if (move) {
        const std::vector<Rect> rects = flow_arrange(sizes, rect.width(), gap_);
        for (std::size_t i = 0; i < placed.size(); ++i) {
            const Rect& r = rects[i];
            placed[i]->setGeometry(QRect(rect.x() + r.x, rect.y() + r.y, r.width, r.height));
        }
    }
    return flow_height(sizes, rect.width(), gap_);
}

} // namespace fv
