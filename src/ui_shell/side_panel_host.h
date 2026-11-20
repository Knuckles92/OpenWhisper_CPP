#pragma once

#include <optional>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QWidget>

class QAbstractAnimation;
class QHBoxLayout;
class QTimer;

namespace fv {

class MainWindow;

// What the main window asks of the context side panel it hosts (OpenWhisper's HistorySidebar:
// toggle, is_expanded and its width animation). The panel's widget implements it beside its Qt
// base. Affinity: GUI thread.
class SidePanel {
public:
    virtual ~SidePanel() = default;
    virtual bool isExpanded() const = 0;
    // Opens a collapsed panel and closes an open one, animating its width.
    virtual void toggle() = 0;
    // The width animation: while it runs the window does not refit to its page, and when it
    // finishes the window refits. Null when the panel does not animate.
    virtual QAbstractAnimation* widthAnimation() const = 0;
};

// What the main window asks of the edge tab that opens and closes the side panel
// (HistoryEdgeTab). A tab that is a QAbstractButton toggles the panel when clicked.
// Affinity: GUI thread.
class SidePanelEdgeTab {
public:
    virtual ~SidePanelEdgeTab() = default;
    virtual void setExpanded(bool expanded) = 0;
    // "History", "Past Meetings" or "Walkthroughs", for its tooltip.
    virtual void setPanelName(const QString& name) = 0;
    virtual void setShortcutHint(const QString& shortcut) = 0;
};

// The main window's side of the context side panel (main_window.py toggle_history,
// _on_sidebar_width_animated, _on_sidebar_animation_finished, the panel name in _on_tab_changed
// and the 75 ms deferred refresh): the window grows by the panel's animated width in lockstep,
// keeping the page's width, and gives the width back as it closes. Where the screen's edge
// stops the window short (the squeeze), closing the panel does not shrink the page.
// Affinity: UI actor (owned by MainWindow).
class SidePanelHost : public QObject {
    Q_OBJECT
public:
    // The refresh after a tab switch waits this long, so tab clicks stay responsive.
    static constexpr int kRefreshDelayMs = 75;

    explicit SidePanelHost(MainWindow& window);

    // The panel name for a tab: "Past Meetings" on Meeting Mode, "Walkthroughs" on Walkthrough,
    // "History" otherwise.
    static QString panelName(int tab);

    // Places edgeTab then panel at the end of layout (beside the tabs). Throws
    // std::invalid_argument when panel is null or not a SidePanel, or edgeTab is neither null
    // nor a SidePanelEdgeTab, and std::logic_error when a panel is already installed.
    void install(QWidget* panel, QWidget* edgeTab, QHBoxLayout* layout);

    QWidget* panel() const { return panel_; }
    QWidget* edgeTab() const { return edgeTab_; }
    bool isExpanded() const;
    bool isAnimating() const;
    // The panel's current (animated) width; 0 without a panel.
    int width() const;

    // toggle_history after its Host Mode and Compact Mode checks.
    void toggle();
    // _on_sidebar_width_animated.
    void widthAnimated(int panelWidth);
    // The edge tab's and the View menu item's names for the tab, and the deferred refresh.
    void tabChanged(int tab);
    // A width the user drags is their own: nothing is owed back to it.
    void resetSqueeze() { squeeze_ = 0; }
    int squeeze() const { return squeeze_; }
    bool refreshPending() const;

private:
    void refreshIfExpanded();

    MainWindow& window_;
    QPointer<QWidget> panel_;
    QPointer<QWidget> edgeTab_;
    SidePanel* sidePanel_ = nullptr;
    SidePanelEdgeTab* sidePanelEdgeTab_ = nullptr;
    QTimer* refreshTimer_;
    // The window's width without the panel, from the last toggle; frames before any toggle
    // are ignored.
    std::optional<int> baseWidth_;
    int squeeze_ = 0;
};

} // namespace fv
