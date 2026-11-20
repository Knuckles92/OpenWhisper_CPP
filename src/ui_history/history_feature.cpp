#include "ui_history/history_feature.h"

#include "remote/record_sync_control.h"
#include "ui_history/history_playback.h"

#include <array>
#include <stdexcept>
#include <utility>

#include <QAction>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "base/clock.h"
#include "base/log.h"
#include "history/calendar_source.h"
#include "history/dictation_stats.h"
#include "history/export.h"
#include "history/manager.h"
#include "settings/store.h"
#include "ui_history/detail/background.h"
#include "ui_history/history_calendar_dialog.h"
#include "ui_history/history_edge_tab.h"
#include "ui_history/history_entry_dialog.h"
#include "ui_history/history_export_dialog.h"
#include "ui_history/history_sidebar.h"
#include "ui_history/history_store.h"
#include "ui_shell/main_window.h"
#include "ui_shell/system_tray.h"
#include "ui_widgets/tabbed_content.h"

namespace fv {
namespace {

constexpr log::Logger kWindowLog{"ui_qt.main_window"};
constexpr log::Logger kLog{"ui_qt.widgets.history_sidebar"};
constexpr log::Logger kControllerLog{"ui_qt.ui_controller"};

// Python's entry.id[:8], for the log.
std::string shortId(const std::string& id) {
    return qt::toUtf8(qt::toQString(id).left(8));
}

} // namespace

HistoryPanel::HistoryPanel(QObject* parent) : QObject(parent) {}

void HistoryPanel::attach(HistorySidebar* sidebar, std::function<void()> toggleInWindow) {
    sidebar_ = sidebar;
    toggleInWindow_ = std::move(toggleInWindow);
}

bool HistoryPanel::isExpanded() const {
    return sidebar_ != nullptr && sidebar_->isExpanded();
}

void HistoryPanel::attachCalendar(std::function<void()> open,
                                  std::function<void()> refreshIfVisible) {
    openCalendar_ = std::move(open);
    refreshCalendar_ = std::move(refreshIfVisible);
}

void HistoryPanel::refresh() {
    if (sidebar_ != nullptr) {
        sidebar_->refresh();
    }
    if (refreshCalendar_) {
        refreshCalendar_();
    }
}

void HistoryPanel::toggle() {
    if (toggleInWindow_) {
        toggleInWindow_();
    }
}

void HistoryPanel::openCalendar() {
    if (openCalendar_) {
        openCalendar_();
    }
}

void HistoryPanel::refreshPastMeetings() {
    if (sidebar_ != nullptr && sidebar_->page() == HistorySidebar::Page::Meetings) {
        sidebar_->refresh();
    }
    if (refreshCalendar_) {
        refreshCalendar_();
    }
}

HistoryPresenter::HistoryPresenter(MainWindow& window, HistoryPanel& panel,
                                   std::shared_ptr<HistoryStore> store, Executor& io,
                                   SettingsStore* settings)
    : QObject(&window), runDialog([](HistoryEntryDialog& dialog) { dialog.exec(); }),
      window_(window), panel_(&panel), store_(std::move(store)), io_(io),
      sidebar_(new HistorySidebar(store_, io_, settings)), edgeTab_(new HistoryEdgeTab) {
    window_.setSidePanel(sidebar_, edgeTab_);
    connect(sidebar_, &HistorySidebar::widthAnimated, &window_,
            &MainWindow::sidePanelWidthAnimated);
    connect(&window_, &MainWindow::sidePanelRefreshDue, sidebar_, &HistorySidebar::refresh);
    connect(&window_, &MainWindow::sidePanelPageRequested, sidebar_,
            [this](int tab) { sidebar_->setPage(HistorySidebar::pageForTab(tab)); });
    if (const TabbedContentWidget* tabs = window_.tabbedContent()) {
        sidebar_->setPage(HistorySidebar::pageForTab(tabs->currentIndex()));
    }

    connect(sidebar_, &HistorySidebar::entrySelected, this, &HistoryPresenter::onEntrySelected);
    connect(sidebar_, &HistorySidebar::entryCopied, this, [this](const QString& entryId) {
        window_.showTransientStatus(QStringLiteral("Copied to clipboard"));
        if (panel_ != nullptr) {
            Q_EMIT panel_->entryCopied(entryId);
        }
    });
    connect(sidebar_, &HistorySidebar::entryDeleted, this, [this](const QString& entryId) {
        refreshCalendar();
        window_.showTransientStatus(QStringLiteral("Entry deleted"));
        if (panel_ != nullptr) {
            Q_EMIT panel_->entryDeleted(entryId);
        }
    });
    connect(sidebar_, &HistorySidebar::retranscribeRequested, this, [this](const QString& path) {
        kWindowLog.info("Re-transcribe requested: {}", qt::toUtf8(path));
        if (panel_ != nullptr) {
            Q_EMIT panel_->retranscribeRequested(path);
        }
    });
    connect(sidebar_, &HistorySidebar::calendarRequested, &panel, &HistoryPanel::calendarRequested);
    connect(sidebar_, &HistorySidebar::calendarRequested, this, &HistoryPresenter::openCalendar);
    connect(sidebar_, &HistorySidebar::exportRequested, &panel, &HistoryPanel::exportRequested);
    connect(sidebar_, &HistorySidebar::exportRequested, this, &HistoryPresenter::openExport);
    runExportDialog = [](HistoryExportDialog& dialog) {
        dialog.exec();
    };
    if (QAction* action = window_.context().menus().action(menu_id::historyCalendar)) {
        connect(action, &QAction::triggered, this, &HistoryPresenter::openCalendar);
    }

    // The panel is a context service and may outlive the window it toggles.
    panel.attach(sidebar_, [presenter = QPointer<HistoryPresenter>(this)] {
        if (presenter != nullptr) {
            presenter->window_.toggleSidePanel();
        }
    });
    panel.attachCalendar(
        [presenter = QPointer<HistoryPresenter>(this)] {
            if (presenter != nullptr) {
                presenter->openCalendar();
            }
        },
        [presenter = QPointer<HistoryPresenter>(this)] {
            if (presenter != nullptr) {
                presenter->refreshCalendar();
            }
        });
}

void HistoryPresenter::setCalendarReads(std::shared_ptr<CalendarReads> reads, const Clock& clock) {
    calendarReads_ = std::move(reads);
    calendarClock_ = &clock;
}

void HistoryPresenter::openCalendar() {
    HistoryCalendarDialog* dialog = calendar_;
    if (dialog == nullptr) {
        if (calendarReads_ == nullptr || calendarClock_ == nullptr) {
            kWindowLog.info("The History calendar has no database yet");
            return;
        }
        dialog =
            new HistoryCalendarDialog(calendarReads_, io_, *calendarClock_, std::nullopt, &window_);
        if (auto* records = sidebar_->remoteRecordActions()) {
            dialog->setPairedMeetingCheckOut(
                [records](std::string_view fingerprint, std::string_view id, const auto& progress) {
                    records->check_out_from("meeting", id, fingerprint, progress);
                });
        }
        connect(dialog, &HistoryCalendarDialog::entryRequested, this,
                [this](const HistoryEntry& entry) { openEntry(entry, calendar_); });
        connect(dialog, &HistoryCalendarDialog::meetingRequested, &window_,
                &MainWindow::pastMeetingRequested);
        connect(dialog, &HistoryCalendarDialog::meetingCopyRequested, &window_,
                &MainWindow::pastMeetingCopyRequested);
        connect(dialog, &HistoryCalendarDialog::entryCopied, this, [this](const QString&) {
            window_.showTransientStatus(QStringLiteral("Copied to clipboard"));
        });
        calendar_ = dialog;
    }
    if (dialog->isVisible()) {
        dialog->refresh();
    }
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    kWindowLog.info("Opened the History calendar");
}

void HistoryPresenter::refreshCalendar() {
    if (calendar_ != nullptr) {
        calendar_->refreshIfVisible();
    }
}

void HistoryPresenter::openExport() {
    if (exporting_) {
        return;
    }
    exporting_ = true;
    if (ui_ == nullptr) {
        ui_ = std::make_shared<qt::QtUiExecutor>(this);
    }
    // What list_export_entries gave, or what it threw: the dialog shows that as its load error.
    struct Read {
        std::vector<Json> entries;
        std::optional<std::string> error;
    };
    history_detail::runInBackground(
        io_, ui_,
        [store = store_, clock = clock_]() -> Read {
            try {
                return {history::list_export_entries(store->get_history(std::nullopt), *clock),
                        std::nullopt};
            } catch (const std::exception& error) {
                return {{}, std::string(error.what())};
            }
        },
        [this](Read read) {
            const struct Reset {
                bool& flag;
                ~Reset() { flag = false; }
            } reset{exporting_};
            auto provider = [read = std::move(read)]() -> std::vector<Json> {
                if (read.error) {
                    throw std::runtime_error(*read.error);
                }
                return read.entries;
            };
            HistoryExportDialog dialog(std::move(provider), store_->recordings_folder(), io_,
                                       sidebar_, *clock_);
            runExportDialog(dialog);
        });
}

void HistoryPresenter::onEntrySelected(const QString& entryId) {
    if (opening_) {
        return;
    }
    opening_ = true;
    // The entry again, or one the paired host keeps for this computer.
    sidebar_->entryFor(entryId, [this](std::optional<HistoryEntry> entry) {
        // Held while the dialog runs: a lookup that ends inside its event loop opens nothing.
        const struct Reset {
            bool& flag;
            ~Reset() { flag = false; }
        } reset{opening_};
        if (entry) {
            openEntry(*entry);
        }
    });
}

void HistoryPresenter::openEntry(const HistoryEntry& entry) {
    openEntry(entry, &window_);
}

void HistoryPresenter::openEntry(const HistoryEntry& entry, QWidget* parent) {
    HistoryEntryDialog dialog(entry, store_, io_, parent != nullptr ? parent : &window_);
    if (entry.stored_on && !entry.stored_on->empty()) {
        if (auto* records = sidebar_->remoteRecordActions()) {
            dialog.playback()->setRemoteAudio(
                io_, [records, id = entry.id] { return records->audio_for(id); });
        }
    }
    connect(&dialog, &HistoryEntryDialog::copied, this, [this] {
        window_.showTransientStatus(QStringLiteral("Copied to clipboard"));
        if (panel_ != nullptr) {
            Q_EMIT panel_->copiedFromDialog();
        }
    });
    connect(&dialog, &HistoryEntryDialog::retranscribeRequested, this, [this](const QString& path) {
        kWindowLog.info("Re-transcribe requested: {}", qt::toUtf8(path));
        if (panel_ != nullptr) {
            Q_EMIT panel_->retranscribeRequested(path);
        }
    });
    connect(&dialog, &HistoryEntryDialog::deleteRequested, this,
            [this, entry](const QString&) { sidebar_->deleteEntryFromDialog(entry); });
    if (!ui_) {
        ui_ = std::make_shared<qt::QtUiExecutor>(this);
    }
    dialog.onVersionSaved = [ui = ui_, this] {
        // The save belongs to history. The dialog may be gone by the time it commits.
        ui->post([this] {
            sidebar_->refresh();
            refreshCalendar();
        });
    };
    runDialog(dialog);
    kWindowLog.info("Opened history entry dialog: {}...", shortId(entry.id));
}

StatsPresenter::StatsPresenter(MainWindow& window, StatsReads reads, Executor& io)
    : QObject(&window), window_(window), reads_(std::move(reads)), io_(io) {
    pastItemsOpener = [this](int tab) {
        showPastItems(tab);
    };
    settingsOpener = [this](const QString& destination) {
        Q_EMIT window_.settingsDestinationRequested(destination);
    };
}

void StatsPresenter::showStats() {
    StatsDialog* dialog = dialog_;
    if (dialog == nullptr) {
        dialog = new StatsDialog(reads_, io_, &window_);
        connect(dialog, &StatsDialog::featureRequested, this, &StatsPresenter::openStatsFeature);
        dialog_ = dialog;
    }
    dialog->showStats();
}

void StatsPresenter::openStatsFeature(const QString& key) {
    static const std::array<std::pair<const char*, int>, 3> kTabs = {{
        {"meetings", TabbedContentWidget::TabMeetingMode},
        {"files", TabbedContentWidget::TabUploadFile},
        {"walkthroughs", TabbedContentWidget::TabWalkthrough},
    }};
    static const std::array<std::pair<const char*, std::string_view>, 4> kPages = {{
        {"actions", destinations::actions},
        {"commands", destinations::commands},
        {"snippets", destinations::snippets},
        {"dictionary", destinations::dictionary},
    }};
    for (const auto& [name, tab] : kTabs) {
        if (key == QLatin1StringView(name)) {
            pastItemsOpener(tab);
            return;
        }
    }
    for (const auto& [name, destination] : kPages) {
        if (key == QLatin1StringView(name)) {
            settingsOpener(qt::toQString(destination));
            return;
        }
    }
}

void StatsPresenter::showPastItems(int tab) {
    window_.restoreFromTray();
    if (TabbedContentWidget* tabs = window_.tabbedContent()) {
        tabs->setCurrentIndex(tab);
    }
    if (!window_.isSidePanelExpanded()) {
        window_.toggleSidePanel();
    }
}

void HistoryFeature::provide(AppContext& context) {
    context.provide<HistoryPanel>(std::make_shared<HistoryPanel>());
}

void HistoryFeature::install(AppContext& context) {
    // Stats count their own table: they need no History store.
    installStats(context);
    std::shared_ptr<HistoryStore> store;
    if (HistoryStore* provided = context.find<HistoryStore>()) {
        // The context outlives every window, so the panel may borrow it.
        store = std::shared_ptr<HistoryStore>(provided, [](HistoryStore*) {});
    } else if (HistoryManager* manager = context.find<HistoryManager>()) {
        store = make_history_store(*manager);
    } else {
        kLog.info("History has no database yet; the History panel is not installed");
        return;
    }
    SettingsStore* settings = nullptr;
    try {
        settings = &context.settings();
    } catch (const ServiceUnavailable&) {
        settings = nullptr;
    }
    auto* presenter =
        new HistoryPresenter(context.service<MainWindow>(), context.service<HistoryPanel>(),
                             std::move(store), context.io(), settings);

    presenter->sidebar()->setRemoteRecords(context.find<history::RemoteRecordSource>(),
                                           context.find<remote::RecordSyncControl>());

    // The calendar's reads: ones the context provides, else its CalendarSource, else one over
    // the database with the meetings and paired-host listings the context provides.
    std::shared_ptr<CalendarReads> reads;
    if (CalendarReads* provided = context.find<CalendarReads>()) {
        reads = std::shared_ptr<CalendarReads>(provided, [](CalendarReads*) {});
    } else if (history::CalendarSource* source = context.find<history::CalendarSource>()) {
        reads = calendarReadsOf(*source);
    } else {
        try {
            reads = calendarReadsOf(std::make_shared<history::CalendarSource>(
                context.db(), context.clock(), context.find<history::MeetingCalendarSource>(),
                context.find<history::RemoteRecordSource>()));
        } catch (const ServiceUnavailable&) {
            kLog.info("History has no database yet; the History calendar is not available");
        }
    }
    if (reads != nullptr) {
        Clock* clock = nullptr;
        try {
            clock = &context.clock();
        } catch (const ServiceUnavailable&) {
            clock = &system_clock();
        }
        presenter->setCalendarReads(std::move(reads), *clock);
    }
    try {
        presenter->setClock(context.clock());
    } catch (const ServiceUnavailable&) {
        presenter->setClock(system_clock());
    }
}

void HistoryFeature::installStats(AppContext& context) {
    auto* stats = context.find<dictation_stats::StatsRepository>();
    if (stats == nullptr) {
        kControllerLog.info("Stats have no database yet; the Stats window is not available");
        return;
    }
    const Clock* clock = &system_clock();
    try {
        clock = &context.clock();
    } catch (const ServiceUnavailable&) {
        clock = &system_clock();
    }
    MainWindow& window = context.service<MainWindow>();
    auto* presenter = new StatsPresenter(window, statsReadsOf(*stats, *clock), context.io());
    // The footer's Stats button and View > Stats (MainWindow::statsRequested), and the tray's
    // Stats, whose action SystemTray turns into its statsRequested.
    QObject::connect(&window, &MainWindow::statsRequested, presenter, &StatsPresenter::showStats);
    if (QAction* tray = context.menus().action(tray_id::stats)) {
        QObject::connect(tray, &QAction::triggered, presenter, &StatsPresenter::showStats);
    }
}

} // namespace fv
