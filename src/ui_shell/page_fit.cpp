#include "ui_shell/page_fit.h"

#include <algorithm>

#include <QApplication>
#include <QEvent>
#include <QLayout>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimer>
#include <QWidget>

#include "ui_shell/main_window.h"
#include "ui_shell/side_panel_host.h"
#include "ui_shell/title_bar.h"
#include "ui_widgets/collapse_animation.h"
#include "ui_widgets/tabbed_content.h"
#include "uimath/window_fit.h"

namespace fv {

QScrollArea* pageScrollArea(QWidget* page) {
    if (page == nullptr) {
        return nullptr;
    }
    if (auto* scroll = qobject_cast<QScrollArea*>(page)) {
        return scroll;
    }
    return page->findChild<QScrollArea*>(Qt::FindDirectChildrenOnly);
}

PageFit::PageFit(MainWindow& window)
    : QObject(&window), window_(window), timer_(new QTimer(this)), floor_(window_size::kMinHeight),
      transcriptHeight_(window_size::kTranscriptionExpandHeight) {
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, [this] { sync(); });
}

// main_window.py:1262-1269: deferring keeps the check off the state-update path and coalesces a
// burst of layout changes into one fit.
void PageFit::schedule() {
    timer_->start(0);
}

bool PageFit::scheduled() const {
    return timer_->isActive();
}

// main_window.py:1271-1279: not deferred, or a page just switched to would show a frame at the
// old height, with a scroll bar it is about to lose.
void PageFit::refit(bool animate) {
    pageFit_.reset();
    widthNeed_.reset();
    sync(animate);
}

void PageFit::watch(QWidget* page) {
    if (QScrollArea* scroll = pageScrollArea(page)) {
        if (QWidget* content = scroll->widget()) {
            content->installEventFilter(this);
        }
    }
}

// _PageLayoutWatcher: any row shown, hidden or rewrapped below the scroll content re-lays it
// out, which posts it a LayoutRequest.
bool PageFit::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::LayoutRequest) {
        schedule();
    }
    return QObject::eventFilter(watched, event);
}

void PageFit::setElastic(QWidget* page, bool elastic) {
    std::erase_if(elastic_, [page](const QPointer<QWidget>& known) {
        return known.isNull() || known.data() == page;
    });
    if (elastic && page != nullptr) {
        elastic_.emplace_back(page);
    }
    schedule();
}

bool PageFit::isElastic(QWidget* page) const {
    if (page == nullptr) {
        return false;
    }
    if (std::ranges::any_of(
            elastic_, [page](const QPointer<QWidget>& known) { return known.data() == page; })) {
        return true;
    }
    // Asked at fit time, as Python asked fills_height(): no page has to announce its transcript.
    return transcriptOpen_ && window_.isTranscriptionTab(page);
}

// main_window.py:1288-1300.
int PageFit::contentHeight(QWidget* page, QScrollArea* scroll, int width) const {
    if (auto* fitted = dynamic_cast<FittedPage*>(page)) {
        // Measure the side-by-side layout a wider window will show, not the stacked one a
        // narrow window still holds, which is far taller.
        fitted->prepareForWidthChange(width - scroll->contentsRect().width());
    }
    QWidget* content = scroll->widget();
    QLayout* layout = content != nullptr ? content->layout() : nullptr;
    if (layout == nullptr) {
        return content != nullptr ? content->sizeHint().height() : 0;
    }
    if (layout->hasHeightForWidth() && width > 0) {
        return std::max(layout->minimumHeightForWidth(width), layout->heightForWidth(width));
    }
    return std::max(layout->minimumSize().height(), layout->sizeHint().height());
}

// main_window.py:1302-1307.
int PageFit::contentWidth(QWidget* page, QScrollArea* scroll) const {
    int need = scroll->widget() != nullptr ? scroll->widget()->minimumSizeHint().width() : 0;
    if (const auto* fitted = dynamic_cast<const FittedPage*>(page)) {
        need = std::max(need, fitted->preferredWidth());
    }
    return need;
}

// main_window.py:1333-1395.
void PageFit::sync(bool animate) {
    QWidget* page = window_.currentPage();
    QScrollArea* scroll = pageScrollArea(page);
    if (window_.isCompactMode() || window_.hostMode_ || !window_.isVisible() ||
        window_.isMaximized() || window_.titleBar()->isMaximized() || window_.isResizing() ||
        scroll == nullptr) {
        return;
    }
    if (animationRunning() || window_.sidePanelHost_->isAnimating()) {
        // Refit once the window settles (onAnimationFinished, the panel's own finish).
        return;
    }

    // Settle pending layouts so the chrome and the page measure as shown.
    QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    const int need = contentWidth(page, scroll);
    const PageWidth widened = page_width(PageWidthInput{need, widthNeed_, window_.width(),
                                                        window_.sidePanelHost_->width(),
                                                        scroll->contentsRect().width(), ownWidth_});
    widthNeed_ = need;
    ownWidth_ = widened.own_width;
    const int width = window_.clampGeometry(0, 0, widened.width, window_.height()).width();
    const int chrome = window_.height() - window_.tabbedContent()->stack()->height();
    // The width the content gets once the window is there, with no scroll bar.
    const int contentWidthThen = scroll->contentsRect().width() + width - window_.width();

    FitInput input;
    input.chrome_height = chrome;
    input.content_height = contentHeight(page, scroll, contentWidthThen);
    input.current_height = window_.height();
    input.available_height = window_.maxUsableHeight();
    input.min_height = window_size::kMinHeight;
    input.elastic = isElastic(page);
    if (page == window_.quickRecordPage()) {
        input.user_sized = quickRecordHeight_.has_value();
        input.user_height = quickRecordHeight_.value_or(0);
    } else {
        input.user_sized = true;
        input.user_height = transcriptHeight_;
    }
    const FitPlan plan = plan_fit(input);
    if (pageFit_ == plan.fit && width == window_.width()) {
        return;
    }
    pageFit_ = plan.fit;
    floor_ = plan.floor;
    // Lower the floor first so a shrink is not stopped short; a rise waits until the window is
    // there, or it would jump.
    window_.setMinimumHeight(plan.interim_min_height);
    if (plan.target != window_.height() || width != window_.width()) {
        if (animate) {
            animateResize(width, plan.target);
            return;
        }
        const QRect geometry = window_.geometry();
        window_.setGeometry(window_.clampGeometry(geometry.x(), geometry.y(), width, plan.target));
    }
    window_.setMinimumHeight(floor_);
}

bool PageFit::animationRunning() const {
    return animation_ != nullptr && animation_->state() == QAbstractAnimation::Running;
}

void PageFit::stopAnimation() {
    if (animationRunning()) {
        animation_->stop();
    }
}

// main_window.py:1915-1945.
void PageFit::animateResize(int width, int height) {
    if (animation_ == nullptr) {
        animation_ = new QPropertyAnimation(&window_, "geometry", this);
        animation_->setDuration(kCollapseDurationMs);
        animation_->setEasingCurve(kCollapseEasing);
        connect(animation_, &QPropertyAnimation::finished, this, &PageFit::onAnimationFinished);
        connect(animation_, &QPropertyAnimation::stateChanged, this,
                [this](QAbstractAnimation::State state, QAbstractAnimation::State) {
                    onAnimationState(state);
                });
    }
    QRect current = window_.geometry();
    const QRect target = window_.clampGeometry(current.x(), current.y(), width, height);
    // Continue smoothly from the current frame when interrupting a resize.
    if (animation_->state() == QAbstractAnimation::Running) {
        current = animation_->currentValue().toRect();
    }
    animation_->stop();
    animation_->setDuration(kCollapseDurationMs);
    animation_->setEasingCurve(kCollapseEasing);
    animation_->setStartValue(current);
    animation_->setEndValue(target);
    animation_->start();
}

// main_window.py:1403-1408.
void PageFit::onAnimationFinished() {
    if (window_.isCompactMode() || window_.hostMode_) {
        return;
    }
    // A transcript toggle moves the window ahead of the floor's refit.
    window_.setMinimumHeight(std::min(floor_, window_.height()));
    schedule();
}

// main_window.py:1947-1960: mid-animation the page is briefly taller than its viewport, which
// would flash a scroll bar the settled window does not need.
void PageFit::onAnimationState(QAbstractAnimation::State state) {
    const bool running = state == QAbstractAnimation::Running;
    QWidget* page = running ? window_.currentPage() : unscrolledPage_.data();
    unscrolledPage_ = running ? page : nullptr;
    if (QScrollArea* scroll = pageScrollArea(page)) {
        scroll->setVerticalScrollBarPolicy(running ? Qt::ScrollBarAlwaysOff
                                                   : Qt::ScrollBarAsNeeded);
    }
}

} // namespace fv
