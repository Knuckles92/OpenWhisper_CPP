#include "ui_widgets/scrolling_body.h"

#include <algorithm>

#include <QDialog>
#include <QLabel>
#include <QLayout>
#include <QScreen>
#include <QVBoxLayout>

namespace fv {

ScrollingBody::ScrollingBody(int maxHeight, QWidget* parent)
    : QScrollArea(parent), maxHeight_(maxHeight) {
    setObjectName(QStringLiteral("dialogScrollingBody"));
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    content_ = new QWidget;
    content_->setObjectName(QStringLiteral("dialogScrollingBodyContent"));
    contentLayout_ = new QVBoxLayout(content_);
    contentLayout_->setContentsMargins(0, 0, 0, 0);
    contentLayout_->setSpacing(12);
    setWidget(content_);
    setMinimumHeight(kMinimumHeight);
}

void ScrollingBody::addWidget(QWidget* widget) {
    contentLayout_->addWidget(widget);
}

int ScrollingBody::contentHeight(int width) const {
    const QMargins margins = contentLayout_->contentsMargins();
    const int inner = width - margins.left() - margins.right();
    int total = 0;
    int shown = 0;
    for (int index = 0; index < contentLayout_->count(); ++index) {
        QWidget* widget = contentLayout_->itemAt(index)->widget();
        if (widget == nullptr || widget->isHidden()) {
            continue;
        }
        const auto* label = qobject_cast<const QLabel*>(widget);
        // QLabel's own measure, not an override's: a WrappedLabel reports its current height.
        total += label != nullptr && label->wordWrap() ? label->QLabel::heightForWidth(inner)
                                                       : widget->sizeHint().height();
        ++shown;
    }
    const int spacing = contentLayout_->spacing() * std::max(0, shown - 1);
    return total + spacing + margins.top() + margins.bottom();
}

void ScrollingBody::fitDialog(QDialog* dialog) {
    QLayout* layout = dialog->layout();
    layout->activate();
    dialog->adjustSize();
    // Place the children so the copy is measured at the width it gets.
    layout->setGeometry(dialog->rect());
    // The viewport is placed only once the window shows, so measure the area itself: with no
    // frame, it is the copy's width while it fits.
    const int width = this->width() - 2 * frameWidth();
    if (width <= 0) {
        return;
    }
    const int needed = contentHeight(width);
    int cap = maxHeight_;
    if (const QScreen* screen = dialog->screen(); screen != nullptr) {
        cap = std::min(cap, screen->availableGeometry().height() - kScreenMargin);
    }
    const int shown = std::max(minimumHeight(), std::min(needed, cap));
    const int target = dialog->height() - height() + shown;
    dialog->resize(dialog->width(), std::max(target, dialog->minimumSizeHint().height()));
}

void fitDialog(QDialog* dialog, ScrollingBody* body) {
    body->fitDialog(dialog);
}

} // namespace fv
