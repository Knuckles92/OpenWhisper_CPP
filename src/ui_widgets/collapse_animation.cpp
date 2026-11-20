#include "ui_widgets/collapse_animation.h"

#include <QEasingCurve>
#include <QPropertyAnimation>
#include <QWidget>

#include <utility>

namespace fv {

QPropertyAnimation* createMaxHeightAnimation(QWidget* widget, QObject* parent) {
    auto* anim = new QPropertyAnimation(widget, "maximumHeight", parent ? parent : widget);
    anim->setDuration(kCollapseDurationMs);
    anim->setEasingCurve(kCollapseEasing);
    return anim;
}

void runMaxHeightAnimation(QPropertyAnimation* anim, int start, int end,
                           std::function<void()> onFinished) {
    anim->stop();
    static_cast<QWidget*>(anim->targetObject())->setMaximumHeight(start);
    anim->setStartValue(start);
    anim->setEndValue(end);
    QObject::disconnect(anim, &QAbstractAnimation::finished, nullptr, nullptr);
    QObject::connect(anim, &QAbstractAnimation::finished, anim, std::move(onFinished),
                     Qt::SingleShotConnection);
    anim->start();
}

} // namespace fv
