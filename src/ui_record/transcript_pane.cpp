#include "ui_record/transcript_pane.h"

namespace fv {

TranscriptPane::TranscriptPane(QWidget* parent) : QFrame(parent) {}

void TranscriptPane::setCornerWidget(QWidget* widget) {
    cornerWidget_ = widget;
    widget->setParent(this);
    widget->raise();
    placeCornerWidget();
}

void TranscriptPane::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    placeCornerWidget();
}

void TranscriptPane::placeCornerWidget() {
    QWidget* widget = cornerWidget_;
    if (widget == nullptr) {
        return;
    }
    const QSize size = widget->size();
    widget->move(width() - size.width() - kCornerInsetX, kCornerInsetY);
    widget->raise();
}

} // namespace fv
