#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include <QMetaMethod>
#include <QObject>
#include <QPointer>
#include <QString>

#include "appkit/feature.h"
#include "base/clock.h"
#include "history/entry.h"
#include "ui_history/history_calendar_dialog.h"
#include "ui_history/history_sidebar.h"
#include "ui_history/stats_dialog.h"

namespace fv {

namespace qt {
class QtUiExecutor;
}

class AppContext;
class CalendarReads;
class Clock;
class Executor;
class HistoryCalendarDialog;
class HistoryEdgeTab;
class HistoryEntryDialog;
class HistoryExportDialog;
class HistoryStore;
class MainWindow;
class SettingsStore;

// The History panel as the rest of the app reaches it (ctx.service<HistoryPanel>()): refresh it
// after a save, open or close it, and hear what it asks for. Provided by HistoryFeature before
// the panel exists; until the panel is installed its calls do nothing.
// Affinity: GUI thread.
class HistoryPanel : public QObject {
    Q_OBJECT
public:
    explicit HistoryPanel(QObject* parent = nullptr);

    // The installed sidebar, or null.
    HistorySidebar* sidebar() const { return sidebar_; }
    void attach(HistorySidebar* sidebar, std::function<void()> toggleInWindow);
    // How the History calendar is opened and refreshed once the presenter exists.
    void attachCalendar(std::function<void()> open, std::function<void()> refreshIfVisible);
    bool isExpanded() const;
    bool hasReceivers(const QMetaMethod& signal) const { return isSignalConnected(signal); }

public Q_SLOTS:
    // MainWindow.refresh_history: refreshes now when open, at the next open otherwise; an open
    // History calendar refreshes too.
    void refresh();
    // The window's panel toggle (View > History, Ctrl+H, the edge tab).
    void toggle();
    // MainWindow.open_history_calendar: shows the one History calendar (View > History
    // Calendar, Ctrl+Shift+D, the calendar button), refreshed when it was already open.
    void openCalendar();
    // MainWindow.refresh_past_meetings: the Past Meetings page when it shows, and the calendar.
    void refreshPastMeetings();

Q_SIGNALS:
    // A card's or the entry dialog's "Transcribe again" / "Retry transcription".
    void retranscribeRequested(const QString& audioPath);
    void entryCopied(const QString& entryId);
    void entryDeleted(const QString& entryId);
    // "Export history…" (the presenter opens the export dialog too).
    void exportRequested();
    // The calendar button (the calendar dialog is M2's).
    void calendarRequested();
    // The entry dialog's Copy, which also shows the copied animation
    // (UIController.on_show_copied_animation).
    void copiedFromDialog();

private:
    QPointer<HistorySidebar> sidebar_;
    std::function<void()> toggleInWindow_;
    std::function<void()> openCalendar_;
    std::function<void()> refreshCalendar_;
};

// The History sidebar wired into the main window (main_window.py's history_sidebar wiring,
// _on_history_entry_selected, _open_history_entry, _on_history_entry_copied(_from_dialog),
// _delete_history_entry, _on_history_entry_deleted, _on_retranscribe_requested): the side panel
// and its edge tab installed through MainWindow::setSidePanel, a card click opening the modal
// entry dialog, statuses for copies and deletes, and the panel's requests relayed through
// HistoryPanel. A child of the window.
// Affinity: GUI thread.
class HistoryPresenter : public QObject {
    Q_OBJECT
public:
    HistoryPresenter(MainWindow& window, HistoryPanel& panel, std::shared_ptr<HistoryStore> store,
                     Executor& io, SettingsStore* settings);

    HistorySidebar* sidebar() const { return sidebar_; }
    HistoryEdgeTab* edgeTab() const { return edgeTab_; }

    // _open_history_entry: the modal entry dialog over the main window.
    void openEntry(const HistoryEntry& entry);
    // The same over parent (the History calendar opens its records above itself).
    void openEntry(const HistoryEntry& entry, QWidget* parent);
    // Runs the entry dialog (exec unless a test drives it).
    std::function<void(HistoryEntryDialog&)> runDialog;

    // What the History calendar reads; without it the calendar does not open. clock gives
    // today and must outlive the presenter.
    void setCalendarReads(std::shared_ptr<CalendarReads> reads, const Clock& clock);
    // MainWindow.open_history_calendar: one calendar, built on first use and reused.
    void openCalendar();
    // _refresh_history_calendar.
    void refreshCalendar();
    // The calendar once it was opened, else null.
    HistoryCalendarDialog* calendar() const { return calendar_; }

    // history_sidebar._on_export_history: the modal export dialog over the sidebar, once every
    // entry was read on io (list_export_entries).
    void openExport();
    // Runs the export dialog (exec unless a test drives it).
    std::function<void(HistoryExportDialog&)> runExportDialog;
    // The clock export entries are stamped and filtered in (system_clock() until set); it must
    // outlive the presenter.
    void setClock(const Clock& clock) { clock_ = &clock; }

private:
    void onEntrySelected(const QString& entryId);

    MainWindow& window_;
    QPointer<HistoryPanel> panel_;
    std::shared_ptr<HistoryStore> store_;
    Executor& io_;
    HistorySidebar* sidebar_;
    HistoryEdgeTab* edgeTab_;
    // An entry is being looked up or shown. OpenWhisper opened the modal dialog inside the
    // click, so a double click's second press met the dialog; the lookup here runs on io, and a
    // click that lands before the dialog shows must not queue a second dialog behind it.
    bool opening_ = false;
    std::shared_ptr<CalendarReads> calendarReads_;
    const Clock* calendarClock_ = nullptr;
    QPointer<HistoryCalendarDialog> calendar_;
    const Clock* clock_ = &system_clock();
    // Delivers the export's read to this presenter while it exists.
    std::shared_ptr<qt::QtUiExecutor> ui_;
    // The entries are being read or the export dialog runs: another request opens nothing.
    bool exporting_ = false;
};

// The Stats window wired into the app (ui_controller.py show_stats and open_stats_feature,
// main_window.py show_past_items): the footer's Stats button, View > Stats and the tray's Stats
// open the one window, and its feature rows open where each feature lives. A child of the
// window.
// Affinity: GUI thread.
class StatsPresenter : public QObject {
    Q_OBJECT
public:
    StatsPresenter(MainWindow& window, StatsReads reads, Executor& io);

    // The window once it was opened (and not deleted since), else null.
    StatsDialog* dialog() const { return dialog_; }
    // show_stats: the one window, made on first use, counted again when already visible, then
    // shown and raised.
    void showStats();
    // open_stats_feature: meetings, files and walkthroughs open their tab with its list of past
    // items; actions, commands, snippets and the dictionary their Settings page. Any other key
    // does nothing.
    void openStatsFeature(const QString& key);
    // MainWindow.show_past_items: the window forward on a tab, with the side panel open.
    void showPastItems(int tab);

    // Where openStatsFeature sends a tab or a Settings destination: showPastItems and the
    // window's settingsDestinationRequested by default.
    std::function<void(int tab)> pastItemsOpener;
    std::function<void(const QString& destination)> settingsOpener;

private:
    MainWindow& window_;
    StatsReads reads_;
    Executor& io_;
    QPointer<StatsDialog> dialog_;
};

// The History feature: provides HistoryPanel, and installs the sidebar into the main window
// once the History store exists (a HistoryStore the context provides, or one over the
// context's HistoryManager). Without either it installs nothing and says so in the log.
class HistoryFeature final : public Feature {
public:
    std::string_view id() const override { return "history"; }
    std::vector<std::string_view> required_features() const override { return {"shell"}; }
    void provide(AppContext& context) override;
    void install(AppContext& context) override;

private:
    // The Stats window, when the context provides the database's StatsRepository.
    static void installStats(AppContext& context);
};

} // namespace fv
