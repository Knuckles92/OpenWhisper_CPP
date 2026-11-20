#include "ui_widgets/tabbed_content.h"

#include <QEvent>
#include <QFont>
#include <QMouseEvent>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>

#include "base/log.h"
#include "pyport/json.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_theme/local_style.h"
#include "ui_theme/style_props.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.tabbed_content"};

constexpr int kSaveDelayMs = 250;

// Python's SAVED_TAB_IDS in its insertion order, which decides a tie between saved values that
// compare equal (a stored true equals the Upload id 1).
constexpr int kTabsInSavedOrder[] = {
    TabbedContentWidget::TabQuickRecord, TabbedContentWidget::TabUploadFile,
    TabbedContentWidget::TabMeetingMode, TabbedContentWidget::TabWalkthrough};

// Meeting Mode is the last tab; [unsupportedMeeting] greys it on unsupported systems without
// setTabEnabled(false), which would steal the current index the moment a meeting is selected.
const QString kTabBarStyle = QStringLiteral(R"(
            QTabBar::tab {
                background-color: transparent;
                color: @text-secondary;
                border: none;
                padding: 12px 12px;
                font-size: 14px;
                font-weight: 600;
                /* Four tabs fit the minimum window width; wider, they stretch. */
                min-width: 104px;
            }
            QTabBar::tab:selected {
                color: @accent;
                border-bottom: 2px solid @accent;
            }
            QTabBar::tab:hover:!selected {
                color: @text;
            }
            QTabBar::tab:disabled {
                color: @text-faint;
            }
            QTabBar[unsupportedMeeting="true"]::tab:last {
                color: @text-faint;
            }
            QTabBar[unsupportedMeeting="true"]::tab:last:hover {
                color: @text-muted;
            }
            QTabBar[unsupportedMeeting="true"]::tab:last:selected {
                color: @accent;
                border-bottom: 2px solid @accent;
            }
        )");

} // namespace

int TabbedContentWidget::savedTabId(int tab) {
    switch (tab) {
    case TabQuickRecord:
        return 0;
    case TabUploadFile:
        return 1;
    case TabMeetingMode:
        return 2;
    case TabWalkthrough:
        return 3;
    default:
        return -1;
    }
}

TabbedContentWidget::TabbedContentWidget(SettingsStore& store, QWidget* parent)
    : QWidget(parent), store_(store) {
    tabSaveTimer_ = new QTimer(this);
    tabSaveTimer_->setSingleShot(true);
    connect(tabSaveTimer_, &QTimer::timeout, this, &TabbedContentWidget::savePendingTabSelection);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    tabBar_ = new QTabBar;
    tabBar_->setObjectName(QStringLiteral("contentTabBar"));
    tabBar_->setFont(QFont(QStringLiteral("Segoe UI"), 13, QFont::DemiBold));
    tabBar_->setDrawBase(false);
    tabBar_->setExpanding(true);
    tabBar_->setUsesScrollButtons(false);

    tabBar_->addTab(QStringLiteral("Quick Record"));
    tabBar_->addTab(QStringLiteral("Upload File"));
    tabBar_->addTab(QStringLiteral("Walkthrough"));
    tabBar_->addTab(QStringLiteral("Meeting Mode"));

    auto* tabContainer = new QWidget;
    auto* tabContainerLayout = new QVBoxLayout(tabContainer);
    tabContainerLayout->setContentsMargins(24, 16, 24, 8);
    tabContainerLayout->setAlignment(Qt::AlignHCenter);
    tabContainerLayout->addWidget(tabBar_, 0, Qt::AlignHCenter);

    layout->addWidget(tabContainer);

    stack_ = new QStackedWidget;
    stack_->setObjectName(QStringLiteral("contentStack"));
    layout->addWidget(stack_, 1);

    ui::setLocalStyleSheet(tabBar_, kTabBarStyle);

    connect(tabBar_, &QTabBar::currentChanged, this, &TabbedContentWidget::onTabChanged);
    tabBar_->installEventFilter(this);

    ui::setStyleProperty(tabBar_, ui::prop::unsupportedMeeting, !meetingSupported_);
    applyMeetingTabTooltip();
    restoreLastTab();
}

void TabbedContentWidget::applyMeetingTabTooltip() {
    if (meetingSupported_) {
        tabBar_->setTabToolTip(
            TabMeetingMode,
            QStringLiteral("Meeting Mode is in beta. Transcripts and insights may be inaccurate."));
        return;
    }
    tabBar_->setTabToolTip(
        TabMeetingMode,
        meetingUnlocked_
            ? QStringLiteral("Meeting Mode is unsupported on %1; system audio is unavailable.")
                  .arg(osName_)
            : QStringLiteral("Meeting Mode is not supported on %1. Click to continue anyway.")
                  .arg(osName_));
}

void TabbedContentWidget::setMeetingTabLocked(bool locked, const QString& osName) {
    if (locked) {
        meetingSupported_ = false;
        meetingUnlocked_ = false;
        osName_ = osName;
    } else {
        meetingUnlocked_ = true;
    }
    ui::setStyleProperty(tabBar_, ui::prop::unsupportedMeeting, !meetingSupported_);
    applyMeetingTabTooltip();
    if (meetingTabIsLocked() && currentIndex() == TabMeetingMode) {
        // A saved Meeting Mode is not restored while it needs its acknowledgement; the saved
        // value stays as it was.
        lastSavedTabIndex_ = TabQuickRecord;
        setCurrentIndex(TabQuickRecord);
    }
}

bool TabbedContentWidget::meetingTabIsLocked() const {
    return !meetingSupported_ && !meetingUnlocked_;
}

void TabbedContentWidget::unlockMeetingTab() {
    meetingUnlocked_ = true;
    applyMeetingTabTooltip();
}

bool TabbedContentWidget::eventFilter(QObject* obj, QEvent* event) {
    if (obj == tabBar_ && (event->type() == QEvent::MouseButtonPress ||
                           event->type() == QEvent::MouseButtonDblClick)) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            const int index = tabBar_->tabAt(mouse->position().toPoint());
            if (index == TabMeetingMode && meetingTabIsLocked()) {
                onLockedMeetingTabActivated();
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void TabbedContentWidget::onLockedMeetingTabActivated() {
    Q_EMIT lockedMeetingTabActivated();
}

void TabbedContentWidget::restoreLastTab() {
    try {
        const Json saved = store_.get(keys::last_tab_index);
        int lastTab = TabQuickRecord;
        for (const int tab : kTabsInSavedOrder) {
            if (py::py_equal(Json(savedTabId(tab)), saved)) {
                lastTab = tab;
                break;
            }
        }
        if (lastTab == TabMeetingMode && meetingTabIsLocked()) {
            lastTab = TabQuickRecord;
        }
        if (lastTab >= 0 && lastTab < tabBar_->count()) {
            lastSavedTabIndex_ = lastTab;
            programmaticTabChange_ = true;
            tabBar_->setCurrentIndex(lastTab);
            programmaticTabChange_ = false;
        }
    } catch (const std::exception& error) {
        programmaticTabChange_ = false;
        kLog.warning("Failed to restore last tab: {}", error.what());
    }
}

void TabbedContentWidget::onTabChanged(int index) {
    if (index == TabMeetingMode && meetingTabIsLocked() && !programmaticTabChange_) {
        int fallback = lastAllowedIndex_;
        if (fallback == TabMeetingMode) {
            fallback = TabQuickRecord;
        }
        tabBar_->blockSignals(true);
        tabBar_->setCurrentIndex(fallback);
        stack_->setCurrentIndex(fallback);
        tabBar_->blockSignals(false);
        onLockedMeetingTabActivated();
        return;
    }

    stack_->setCurrentIndex(index);
    lastAllowedIndex_ = index;

    scheduleTabSelectionSave(index);

    Q_EMIT currentChanged(index);
    kLog.debug("Tab changed to index {}", index);
}

void TabbedContentWidget::scheduleTabSelectionSave(int index) {
    // Persist after the UI has had time to switch.
    if (lastSavedTabIndex_ && index == *lastSavedTabIndex_) {
        pendingTabIndex_.reset();
        if (tabSaveTimer_->isActive()) {
            tabSaveTimer_->stop();
        }
        return;
    }

    pendingTabIndex_ = index;
    tabSaveTimer_->start(kSaveDelayMs);
}

void TabbedContentWidget::savePendingTabSelection() {
    if (!pendingTabIndex_) {
        return;
    }

    const int index = *pendingTabIndex_;
    pendingTabIndex_.reset();

    try {
        store_.save_setting(keys::last_tab_index.key(), savedTabId(index));
        lastSavedTabIndex_ = index;
    } catch (const std::exception& error) {
        kLog.warning("Failed to save tab selection: {}", error.what());
    }
}

void TabbedContentWidget::flushPendingTabSave() {
    if (tabSaveTimer_->isActive()) {
        tabSaveTimer_->stop();
    }
    savePendingTabSelection();
}

void TabbedContentWidget::setPage(int index, QWidget* page) {
    while (stack_->count() < index) {
        stack_->addWidget(new QWidget);
    }
    if (index < stack_->count()) {
        QWidget* old = stack_->widget(index);
        stack_->removeWidget(old);
        old->deleteLater();
    }
    stack_->insertWidget(index, page);
    kLog.debug("Added page at index {}", index);
    syncStackWithTabBar();
}

void TabbedContentWidget::syncStackWithTabBar() {
    const int currentTab = tabBar_->currentIndex();
    if (stack_->currentIndex() != currentTab) {
        kLog.debug("Syncing stack (was {}) with tab bar (index {})", stack_->currentIndex(),
                   currentTab);
        stack_->setCurrentIndex(currentTab);
    }
}

int TabbedContentWidget::currentIndex() const {
    return tabBar_->currentIndex();
}

void TabbedContentWidget::setCurrentIndex(int index) {
    if (index >= 0 && index < tabBar_->count()) {
        programmaticTabChange_ = true;
        tabBar_->setCurrentIndex(index);
        programmaticTabChange_ = false;
    }
}

void TabbedContentWidget::setRecordingState(bool isRecording, int sourceTab) {
    if (isRecording && sourceTab >= 0 && sourceTab < tabBar_->count()) {
        // Disabling the selected tab lets Qt choose a neighbour, which can briefly visit (and
        // queue a save for) Meeting Mode during dictation.
        tabBar_->setTabEnabled(sourceTab, true);
        setCurrentIndex(sourceTab);
    }
    for (int i = 0; i < tabBar_->count(); ++i) {
        tabBar_->setTabEnabled(i, !isRecording || i == sourceTab);
    }

    kLog.debug("Recording state: active={}, source_tab={}", isRecording, sourceTab);
}

} // namespace fv
