#pragma once

// Stand-ins for the widgets later features plug into the main window: a side panel and its
// edge tab (the History sidebar's shape), a tab page with Quick Record's scroll-area structure,
// a screen and a compact controller that record what the window relays to them.

#include <utility>
#include <vector>

#include <QAbstractAnimation>
#include <QApplication>
#include <QFrame>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QString>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include "shell_fixture.h"
#include "ui_shell/main_window.h"
#include "ui_shell/page_fit.h"
#include "ui_shell/side_panel_host.h"
#include "ui_widgets/collapse_animation.h"
#include "uimath/window_geometry.h"

namespace fvtest {

// settle(), then until the window has no fit pending, no resize running and no panel moving: on
// a loaded machine the 800 ms can end before an animation does.
inline void settleWindow(fv::MainWindow& window) {
    settle();
    waitUntil([&window] {
        const auto* panel = dynamic_cast<const fv::SidePanel*>(window.sidePanel());
        const QAbstractAnimation* moving = panel != nullptr ? panel->widthAnimation() : nullptr;
        return !window.pageFit().scheduled() && !window.pageFit().animationRunning() &&
               (moving == nullptr || moving->state() != QAbstractAnimation::Running);
    });
    QApplication::processEvents();
}

// HistorySidebar's width machinery: 0 px collapsed, 380 px expanded, animated over 300 ms, with
// every frame announced through widthAnimated.
class StandInPanel : public QWidget, public fv::SidePanel {
    Q_OBJECT
    Q_PROPERTY(int sidebarWidth READ sidebarWidth WRITE setSidebarWidth)
public:
    static constexpr int kExpandedWidth = fv::window_size::kHistorySidebarWidth;

    explicit StandInPanel(QWidget* parent = nullptr)
        : QWidget(parent), animation_(new QPropertyAnimation(this, "sidebarWidth", this)) {
        setFixedWidth(0);
        animation_->setDuration(fv::kCollapseDurationMs);
        animation_->setEasingCurve(fv::kCollapseEasing);
        connect(animation_, &QPropertyAnimation::finished, this,
                [this] { setFixedWidth(expanded_ ? kExpandedWidth : 0); });
    }

    bool isExpanded() const override { return expanded_; }
    void toggle() override {
        expanded_ = !expanded_;
        animation_->stop();
        animation_->setStartValue(width_);
        animation_->setEndValue(expanded_ ? kExpandedWidth : 0);
        animation_->start();
    }
    QAbstractAnimation* widthAnimation() const override { return animation_; }

    int sidebarWidth() const { return width_; }
    void setSidebarWidth(int width) {
        width_ = width;
        setFixedWidth(width);
        Q_EMIT widthAnimated(width);
    }

    int refreshes = 0;
    std::vector<int> pagesRequested;

public Q_SLOTS:
    void refresh() { ++refreshes; }
    void showPageFor(int tab) { pagesRequested.push_back(tab); }

Q_SIGNALS:
    void widthAnimated(int width);

private:
    QPropertyAnimation* animation_;
    int width_ = 0;
    bool expanded_ = false;
};

// HistoryEdgeTab's interface, keeping what the window told it.
class StandInEdgeTab : public QPushButton, public fv::SidePanelEdgeTab {
    Q_OBJECT
public:
    StandInEdgeTab() { setFixedWidth(fv::window_size::kHistoryEdgeTabWidth); }

    void setExpanded(bool expanded) override { expanded_ = expanded; }
    void setPanelName(const QString& name) override { name_ = name; }
    void setShortcutHint(const QString& shortcut) override { shortcut_ = shortcut; }

    bool expanded() const { return expanded_; }
    QString panelName() const { return name_; }
    QString shortcutHint() const { return shortcut_; }

private:
    bool expanded_ = false;
    QString name_;
    QString shortcut_;
};

// A tab page with Quick Record's structure: a QVBoxLayout holding a resizable QScrollArea whose
// content stacks fixed-height rows. It takes the calls the window relays to Quick Record.
class StandInPage : public QWidget {
    Q_OBJECT
public:
    explicit StandInPage(std::vector<int> rowHeights, QWidget* parent = nullptr)
        : QWidget(parent), scroll_(new QScrollArea) {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        scroll_->setWidgetResizable(true);
        scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll_->setFrameShape(QFrame::NoFrame);
        auto* content = new QWidget;
        content_ = new QVBoxLayout(content);
        content_->setContentsMargins(16, 16, 16, 16);
        content_->setSpacing(12);
        for (const int height : rowHeights) {
            addRow(height);
        }
        content_->addStretch();
        scroll_->setWidget(content);
        layout->addWidget(scroll_);
    }

    QScrollArea* scrollArea() const { return scroll_; }
    // A row shown below the others, as a result card appears.
    QWidget* addRow(int height) {
        auto* row = new QFrame;
        row->setFixedHeight(height);
        content_->insertWidget(rows_++, row);
        return row;
    }

    QStringList calls;

public Q_SLOTS:
    void setStatus(const QString& status) { calls << QStringLiteral("setStatus:") + status; }
    void setRecordingActive(bool active) {
        calls << QStringLiteral("setRecordingActive:%1").arg(active);
    }
    void setTranscriptionCollapsed(bool collapsed) {
        calls << QStringLiteral("setTranscriptionCollapsed:%1").arg(collapsed);
    }

Q_SIGNALS:
    void transcriptionCollapsed(bool collapsed, int delta);

private:
    QScrollArea* scroll_;
    QVBoxLayout* content_;
    int rows_ = 0;
};

// A screen of another tab (Upload File, Walkthrough, Meeting Mode) that records the recording
// state the window relays.
class StandInScreen : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
    QStringList calls;

public Q_SLOTS:
    void setRecordingActive(bool active) {
        calls << QStringLiteral("setRecordingActive:%1").arg(active);
    }
    void setRecordingState(bool recording, bool walkthrough) {
        calls << QStringLiteral("setRecordingState:%1,%2").arg(recording).arg(walkthrough);
    }
};

// CompactRecordController's setters.
class StandInController : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
    QStringList calls;

public Q_SLOTS:
    void setRecordingState(bool recording) {
        calls << QStringLiteral("setRecordingState:%1").arg(recording);
    }
    void setStatus(const QString& status) { calls << QStringLiteral("setStatus:") + status; }
};

} // namespace fvtest
