#include "ui_widgets/reflow.h"

#include <algorithm>

#include <QGridLayout>
#include <QSizePolicy>

#include "ui_theme/font_scale.h"

namespace fv {
namespace {

// A label-fitted button can hold a floor above its size hint.
int naturalWidth(const QWidget* widget) {
    return std::max(widget->sizeHint().width(), widget->minimumWidth());
}

} // namespace

Reflow::Reflow(QWidget* lead, QWidget* actions, std::optional<int> leadWidth, QWidget* parent,
               Qt::Alignment stackedAlignment)
    : QWidget(parent), lead_(lead), actions_(actions), leadWidth_(leadWidth),
      stackedAlignment_(stackedAlignment) {
    setObjectName(QStringLiteral("reflow"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    grid_ = new QGridLayout(this);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setHorizontalSpacing(8);
    grid_->setVerticalSpacing(6);
    grid_->addWidget(lead, 0, 0);
    grid_->setColumnStretch(0, 1);
    place(false);
}

void Reflow::place(bool stacked) {
    if (stacked_ == stacked) {
        return;
    }
    stacked_ = stacked;
    grid_->removeWidget(actions_);
    if (stacked) {
        grid_->addWidget(actions_, 1, 0, stackedAlignment_);
    } else {
        grid_->addWidget(actions_, 0, 1, Qt::AlignVCenter);
    }
}

void Reflow::reflow() {
    const int lead = leadWidth_ ? ui::scaled(*leadWidth_) : naturalWidth(lead_);
    const int needed = lead + grid_->horizontalSpacing() + naturalWidth(actions_);
    place(width() < needed);
}

void Reflow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    reflow();
}

} // namespace fv
