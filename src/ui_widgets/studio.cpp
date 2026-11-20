#include "ui_widgets/studio.h"

#include <vector>

#include <QBoxLayout>
#include <QSizePolicy>

#include "ui_theme/font_scale.h"

namespace fv {

Stage::Stage(const QString& objectName, int width, std::optional<int> height)
    : width_(width), height_(height) {
    setObjectName(objectName);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

QWidget* Stage::addPane(QWidget* pane) {
    pane->setParent(this);
    pane->hide();
    panes_.push_back(pane);
    return pane;
}

void Stage::showPane(QWidget* pane) {
    current_ = pane;
    pane->setGeometry(rect());
    std::erase_if(panes_, [](const QPointer<QWidget>& each) { return each.isNull(); });
    for (const QPointer<QWidget>& each : panes_) {
        each->setVisible(each == pane);
    }
}

void Stage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    std::erase_if(panes_, [](const QPointer<QWidget>& each) { return each.isNull(); });
    for (const QPointer<QWidget>& pane : panes_) {
        pane->setGeometry(rect());
    }
}

int Stage::stageHeight() const {
    return height_ ? ui::scaled(*height_) : studio::kStageMin.height();
}

QSize Stage::sizeHint() const {
    return {ui::scaled(width_), stageHeight()};
}

QSize Stage::minimumSizeHint() const {
    return {ui::scaled(studio::kStageMin.width()), stageHeight()};
}

StudioSplit::StudioSplit(QWidget* column, Stage* stage, QWidget* parent, const QString& objectName)
    : QWidget(parent), column_(column), stage_(stage) {
    setObjectName(objectName);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    box_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
    box_->setContentsMargins(0, 0, 0, 0);
    box_->setSpacing(studio::kGap);
    box_->addWidget(column);
    box_->addWidget(stage, 1);
    place(false);
}

void StudioSplit::setColumnWidth(int width) {
    columnWidth_ = width;
    stacked_.reset();
    refit();
}

void StudioSplit::place(bool stacked) {
    if (stacked_ == stacked) {
        return;
    }
    stacked_ = stacked;
    box_->setDirection(stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    if (stacked) {
        column_->setMinimumWidth(0);
        column_->setMaximumWidth(QWIDGETSIZE_MAX);
        column_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    } else {
        column_->setFixedWidth(columnWidth_);
        column_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    }
    updateGeometry();
}

void StudioSplit::refit() {
    placeForWidth(width());
}

void StudioSplit::placeForWidth(int width) {
    const int needed = columnWidth_ + studio::kGap + stage_->minimumSizeHint().width();
    place(0 < width && width < needed);
}

void StudioSplit::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    refit();
}

} // namespace fv
