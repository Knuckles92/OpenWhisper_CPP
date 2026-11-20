#pragma once

#include <optional>

#include <QString>
#include <QWidget>

class QStackedWidget;
class QTabBar;
class QTimer;

namespace fv {

class SettingsStore;

// Main-window tab container: a tab bar (object name "contentTabBar") over a stacked content area
// ("contentStack"). Remembers the selected tab in settings (last_tab_index), saved 250 ms after
// a change and on flushPendingTabSave. Affinity: GUI thread.
class TabbedContentWidget : public QWidget {
    Q_OBJECT
public:
    static constexpr int TabQuickRecord = 0;
    static constexpr int TabUploadFile = 1;
    static constexpr int TabWalkthrough = 2;
    // Last, so the theme's ::tab:last rules grey it on unsupported platforms.
    static constexpr int TabMeetingMode = 3;

    // What last_tab_index stores for each tab: the positions the tabs had before Walkthrough
    // joined, so a saved tab still opens the same one (Quick 0, Upload 1, Meeting 2,
    // Walkthrough 3).
    static int savedTabId(int tab);

    // store outlives the widget.
    explicit TabbedContentWidget(SettingsStore& store, QWidget* parent = nullptr);

    QTabBar* tabBar() const { return tabBar_; }
    QStackedWidget* stack() const { return stack_; }

    // Puts page at index (the position of its tab), replacing and deleting the page there; gaps
    // below index are filled with empty pages.
    void setPage(int index, QWidget* page);
    // Makes the stack show the tab bar's current tab.
    void syncStackWithTabBar();

    int currentIndex() const;
    // Selects a tab without counting as the user's choice; an index out of range is ignored.
    void setCurrentIndex(int index);

    // While recording, only sourceTab stays enabled (and selected); false enables every tab.
    void setRecordingState(bool isRecording, int sourceTab);

    // Synchronously persists any queued tab selection.
    void flushPendingTabSave();

    // The gate for platforms where Meeting Mode is unsupported: a locked Meeting Mode tab cannot
    // be selected by clicking it; the click emits lockedMeetingTabActivated instead, and the
    // owner calls unlockMeetingTab once the user accepts the warning. osName names the platform
    // in the tooltips. Wired by the meeting feature.
    void setMeetingTabLocked(bool locked, const QString& osName = {});
    bool meetingTabIsLocked() const;
    void unlockMeetingTab();

Q_SIGNALS:
    void currentChanged(int index);
    // The user clicked, or selected by key, the locked Meeting Mode tab.
    void lockedMeetingTabActivated();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void restoreLastTab();
    void onTabChanged(int index);
    void onLockedMeetingTabActivated();
    void applyMeetingTabTooltip();
    void scheduleTabSelectionSave(int index);
    void savePendingTabSelection();

    SettingsStore& store_;
    QTabBar* tabBar_;
    QStackedWidget* stack_;
    QTimer* tabSaveTimer_;

    std::optional<int> lastSavedTabIndex_;
    std::optional<int> pendingTabIndex_;
    int lastAllowedIndex_ = TabQuickRecord;
    bool programmaticTabChange_ = false;
    bool meetingSupported_ = true;
    bool meetingUnlocked_ = true;
    QString osName_;
};

} // namespace fv
