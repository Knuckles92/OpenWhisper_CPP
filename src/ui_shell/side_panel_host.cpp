#include "ui_shell/side_panel_host.h"

#include <algorithm>
#include <stdexcept>

#include <QAbstractAnimation>
#include <QAction>
#include <QHBoxLayout>
#include <QTimer>

#include "appkit/app_context.h"
#include "appkit/registries.h"
#include "ui_shell/main_window.h"
#include "ui_widgets/tabbed_content.h"
#include "uimath/window_fit.h"

namespace fv {

SidePanelHost::SidePanelHost(MainWindow& window)
    : QObject(&window), window_(window), refreshTimer_(new QTimer(this)) {
    refreshTimer_->setSingleShot(true);
    connect(refreshTimer_, &QTimer::timeout, this, &SidePanelHost::refreshIfExpanded);
}

// main_window.py:1054-1057: Meeting Mode and the Walkthrough tab list their own past items.
QString SidePanelHost::panelName(int tab) {
    switch (tab) {
    case TabbedContentWidget::TabMeetingMode:
        return QStringLiteral("Past Meetings");
    case TabbedContentWidget::TabWalkthrough:
        return QStringLiteral("Walkthroughs");
    default:
        return QStringLiteral("History");
    }
}

void SidePanelHost::install(QWidget* panel, QWidget* edgeTab, QHBoxLayout* layout) {
    if (panel_ != nullptr) {
        throw std::logic_error("The main window already has a side panel.");
    }
    auto* sidePanel = dynamic_cast<SidePanel*>(panel);
    if (sidePanel == nullptr) {
        throw std::invalid_argument("The side panel must be a widget implementing fv::SidePanel.");
    }
    auto* sidePanelEdgeTab = dynamic_cast<SidePanelEdgeTab*>(edgeTab);
    if (edgeTab != nullptr && sidePanelEdgeTab == nullptr) {
        throw std::invalid_argument(
            "The side panel's edge tab must be a widget implementing fv::SidePanelEdgeTab.");
    }
    panel_ = panel;
    sidePanel_ = sidePanel;
    edgeTab_ = edgeTab;
    sidePanelEdgeTab_ = sidePanelEdgeTab;
    if (edgeTab != nullptr) {
        sidePanelEdgeTab->setShortcutHint(QString::fromLatin1(MainWindow::kHistoryShortcut));
        const TabbedContentWidget* tabs = window_.tabbedContent();
        sidePanelEdgeTab->setPanelName(panelName(tabs != nullptr ? tabs->currentIndex() : 0));
        layout->addWidget(edgeTab);
    }
    layout->addWidget(panel);
}

bool SidePanelHost::isExpanded() const {
    return panel_ != nullptr && sidePanel_->isExpanded();
}

bool SidePanelHost::isAnimating() const {
    if (panel_ == nullptr) {
        return false;
    }
    const QAbstractAnimation* animation = sidePanel_->widthAnimation();
    return animation != nullptr && animation->state() == QAbstractAnimation::Running;
}

int SidePanelHost::width() const {
    return panel_ != nullptr ? panel_->width() : 0;
}

bool SidePanelHost::refreshPending() const {
    return refreshTimer_->isActive();
}

// main_window.py:1809-1828.
void SidePanelHost::toggle() {
    if (panel_ == nullptr) {
        return;
    }
    const bool willBeExpanded = !sidePanel_->isExpanded();
    if (edgeTab_ != nullptr) {
        sidePanelEdgeTab_->setExpanded(willBeExpanded);
    }
    // Everything but the panel, held at a constant width through the animation's frames. Works
    // mid-animation too: the panel's current width is subtracted out.
    baseWidth_ = window_.width() - panel_->width() + squeeze_;
    // The panel's single animation drives the window's width through widthAnimated.
    sidePanel_->toggle();
}

// main_window.py:1836-1852.
void SidePanelHost::widthAnimated(int panelWidth) {
    if (!baseWidth_) {
        return;
    }
    const PanelStep step = side_panel_step(*baseWidth_, panelWidth);
    if (step.limit > window_.maximumWidth()) {
        window_.setMaximumWidth(step.limit);
    }
    const QRect geometry = window_.geometry();
    const QRect clamped =
        window_.clampGeometry(geometry.x(), geometry.y(), step.wanted, geometry.height());
    window_.setGeometry(clamped);
    window_.setMaximumWidth(step.limit);
    squeeze_ = std::max(0, step.wanted - clamped.width());
}

// main_window.py:1058-1063.
void SidePanelHost::tabChanged(int tab) {
    const QString name = panelName(tab);
    if (edgeTab_ != nullptr) {
        sidePanelEdgeTab_->setPanelName(name);
    }
    if (QAction* action = window_.context().menus().action(menu_id::sidePanel)) {
        action->setText(name);
    }
    // _schedule_history_sidebar_refresh: deferred so tab clicks stay responsive.
    if (isExpanded()) {
        refreshTimer_->start(kRefreshDelayMs);
    }
}

void SidePanelHost::refreshIfExpanded() {
    if (isExpanded()) {
        Q_EMIT window_.sidePanelRefreshDue();
    }
}

} // namespace fv
