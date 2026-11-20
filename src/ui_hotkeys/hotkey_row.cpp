#include "ui_hotkeys/hotkey_row.h"

#include <algorithm>

#include <QBoxLayout>
#include <QHBoxLayout>
#include <QResizeEvent>
#include <QVBoxLayout>

#include "ui_theme/font_scale.h"

namespace fv {

ReflowCard::ReflowCard(QWidget* parent) : QFrame(parent) {
    outer_ = new QVBoxLayout(this);
    outer_->setContentsMargins(14, 8, 12, 8);
    outer_->setSpacing(8);
    box_ = new QBoxLayout(QBoxLayout::LeftToRight);
    box_->setSpacing(16);
    copy_ = new QVBoxLayout();
    copy_->setSpacing(2);
    controls_ = new QHBoxLayout();
    controls_->setSpacing(8);
    box_->addLayout(copy_, 1);
    box_->addLayout(controls_);
    outer_->addLayout(box_);
}

void ReflowCard::addControl(QWidget* widget) {
    controls_->addWidget(widget, 0, Qt::AlignVCenter);
    controlWidgets_.emplace_back(widget, widget->minimumWidth());
}

void ReflowCard::addFooter(QWidget* widget) {
    outer_->addWidget(widget);
}

void ReflowCard::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    reflow(event->size().width());
}

void ReflowCard::reflow(int width) {
    const QMargins margins = outer_->contentsMargins();
    const int inner = width - margins.left() - margins.right();
    std::vector<int> widths;
    int total = 0;
    for (const auto& [widget, minimum] : controlWidgets_) {
        widths.push_back(std::max(minimum, widget->sizeHint().width()));
        total += widths.back();
    }
    const int spacing = controls_->spacing() * std::max(0, static_cast<int>(widths.size()) - 1);
    const bool stacked = inner < ui::scaled(kMinCopyWidth) + 16 + total + spacing;
    if (stacked != stacked_) {
        stacked_ = stacked;
        box_->setDirection(stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
        box_->setSpacing(stacked ? 8 : 16);
        box_->setAlignment(controls_, stacked ? Qt::AlignLeft : Qt::Alignment());
        updateGeometry();
    }
    if (!controlWidgets_.empty()) {
        const auto& [first, minimum] = controlWidgets_.front();
        const int room = inner - (total - widths.front()) - spacing;
        first->setMinimumWidth(stacked ? std::max(0, std::min(minimum, room)) : minimum);
    }
}

} // namespace fv
