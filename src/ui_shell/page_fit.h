#pragma once

#include <optional>
#include <vector>

#include <QAbstractAnimation>
#include <QObject>
#include <QPointer>

class QPropertyAnimation;
class QScrollArea;
class QTimer;
class QWidget;

namespace fv {

class MainWindow;

// What a tab page may tell the fit beyond its scroll area (the duck-typed preferred_width and
// split of OpenWhisper's Walkthrough and Meeting tabs). A page implements it beside its Qt base;
// pages without it are measured by their scroll area alone. Affinity: GUI thread.
class FittedPage {
public:
    virtual ~FittedPage() = default;
    // The content width the page reads best at (preferred_width); 0 when its minimum is enough.
    virtual int preferredWidth() const { return 0; }
    // The page's scroll-area contents are about to be `grow` pixels wider (or narrower): a page
    // with a side-by-side split lays it out for that width now, so it is measured as it will
    // show rather than stacked (split.place_for_width).
    virtual void prepareForWidthChange(int grow) { (void)grow; }
};

// The scroll area a page's content lives in: the page itself, or the first QScrollArea among
// its direct children (OpenWhisper's page.scroll_area). Null for a page that has none, which
// the window then leaves at the size the user gave it.
QScrollArea* pageScrollArea(QWidget* page);

// Fits the main window to the open tab's page (main_window.py:1262-1420, 1915-1961): the
// height shows the whole page without scrolling, capped at the screen; the floor is
// min(fit, 660); an elastic page (open transcript, storyboard) keeps the height the user drags;
// a page that needs more width widens the window and the width is handed back to the next page
// that fits. Changes are animated over 300 ms (InOutCubic) with the page's scroll bar held off,
// and a burst of layout changes is coalesced into one fit on the next event-loop pass.
// Affinity: UI actor (owned by MainWindow).
class PageFit : public QObject {
    Q_OBJECT
public:
    explicit PageFit(MainWindow& window);

    // _schedule_page_fit: fits on the next event-loop pass.
    void schedule();
    bool scheduled() const;
    // _refit_page: fits now, even when the page's size has not changed.
    void refit(bool animate = true);
    // _sync_page_size.
    void sync(bool animate = true);

    // _animate_resize: animates the window's geometry to width x height, clamped to the
    // screen, continuing from the current frame when a resize is under way.
    void animateResize(int width, int height);
    bool animationRunning() const;
    void stopAnimation();
    QPropertyAnimation* animation() const { return animation_; }

    // Refits whenever the page's scroll content asks for a new layout (_PageLayoutWatcher), so
    // no page has to announce its own changes.
    void watch(QWidget* page);
    void setElastic(QWidget* page, bool elastic);
    // A page set elastic, or a transcription tab (Quick Record and the screens of
    // kTranscriptionGroup) while its transcript is open: TranscriptionTabBase.fills_height.
    bool isElastic(QWidget* page) const;
    // The transcription tabs share one collapsed state (MainWindow::onTranscriptionCollapsed);
    // their transcripts start collapsed, as the card does on launch.
    void setTranscriptOpen(bool open) { transcriptOpen_ = open; }
    bool transcriptOpen() const { return transcriptOpen_; }

    int floor() const { return floor_; }
    std::optional<int> ownWidth() const { return ownWidth_; }
    int transcriptHeight() const { return transcriptHeight_; }
    void setTranscriptHeight(int height) { transcriptHeight_ = height; }
    std::optional<int> quickRecordHeight() const { return quickRecordHeight_; }
    void setQuickRecordHeight(int height) { quickRecordHeight_ = height; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    int contentHeight(QWidget* page, QScrollArea* scroll, int width) const;
    int contentWidth(QWidget* page, QScrollArea* scroll) const;
    void onAnimationFinished();
    void onAnimationState(QAbstractAnimation::State state);

    MainWindow& window_;
    QTimer* timer_;
    QPropertyAnimation* animation_ = nullptr;
    std::vector<QPointer<QWidget>> elastic_;
    bool transcriptOpen_ = false;
    // The page whose scroll bar a running animation holds off.
    QPointer<QWidget> unscrolledPage_;
    // The page height the window last fitted, so it refits only on change.
    std::optional<int> pageFit_;
    // The content width the page last asked for, so it rewidens only on change.
    std::optional<int> widthNeed_;
    std::optional<int> ownWidth_;
    int floor_;
    int transcriptHeight_;
    // Quick Record fits the wrapped transcript until the user drags its height.
    std::optional<int> quickRecordHeight_;
};

} // namespace fv
