#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QMessageBox>
#include <QMetaMethod>
#include <QPointer>
#include <QString>
#include <QWidget>

#include "appkit/qt_bridge.h"
#include "base/json.h"
#include "history/entry.h"
#include "ui_shell/side_panel_host.h"
#include "ui_widgets/list_reconcile.h"
#include "uimath/window_geometry.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QMessageBox;
class QPropertyAnimation;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace fv {

class CalendarGlyphButton;
class Clock;
class Executor;
class HistoryItemWidget;
class HistoryStore;
class SettingsStore;
namespace history {
class RemoteRecordSource;
}
namespace remote {
class RecordSyncControl;
}

// The slide-out History panel beside the main window's tabs (history_sidebar.py
// HistorySidebar): a header with its menu and the calendar button, a search field, and the
// newest 100 entries as cards. It animates one sidebarWidth property and announces every frame
// (widthAnimated) so the window resizes in lockstep; the content is a fixed-width child pinned
// in resizeEvent rather than laid out, so animating the width only clips and reveals content
// that is already laid out.
//
// The list loads on the io executor, newest request winning (a generation token); results come
// back through an executor bound to the sidebar, so a load that ends after the sidebar is gone
// is dropped and no worker ever holds or emits on a widget. Every other History call (open,
// copy, delete, a version, clearing) goes the same way.
//
// The sidebar has three pages, one for each tab family: History, Past Meetings (Meeting Mode)
// and Walkthroughs. The last two belong to M7 and M6, which hand it their panels.
// Affinity: GUI thread.
class HistorySidebar : public QWidget, public SidePanel {
    Q_OBJECT
    Q_PROPERTY(int sidebarWidth READ sidebarWidth WRITE setSidebarWidth)
public:
    enum class Page { History, Meetings, Walkthroughs };

    static constexpr int kCollapsedWidth = 0;
    static constexpr int kExpandedWidth = window_size::kHistorySidebarWidth;
    // REMOTE_MERGE_WAIT_S: how long a load waits for the host's entries before it shows this
    // computer's alone; the host's are merged in when they come.
    static constexpr std::chrono::milliseconds kRemoteMergeWait{400};
    // Cards rendered at most; search still filters the whole history.
    static constexpr int kMaxHistoryItems = 100;
    // Typing restarts the search this long after the last key.
    static constexpr int kSearchDelayMs = 250;

    // store and io must outlive the sidebar. settings (may be null) holds
    // confirm_history_entry_delete; clock gives the calendar button its day.
    HistorySidebar(std::shared_ptr<HistoryStore> store, Executor& io, SettingsStore* settings,
                   QWidget* parent = nullptr);
    HistorySidebar(std::shared_ptr<HistoryStore> store, Executor& io, SettingsStore* settings,
                   const Clock& clock, QWidget* parent = nullptr);
    ~HistorySidebar() override;

    // SidePanel.
    void setRemoteRecords(history::RemoteRecordSource*, remote::RecordSyncControl*);
    // How long a load waits for the host (kRemoteMergeWait unless a test shortens it).
    void setRemoteMergeWait(std::chrono::milliseconds wait) { remoteMergeWait_ = wait; }
    remote::RecordSyncControl* remoteRecordActions() const { return remoteActions_; }
    bool isExpanded() const override { return expanded_; }
    void toggle() override;
    QAbstractAnimation* widthAnimation() const override;

    // The first open fills the list before the panel slides out.
    void expand();
    // Stops any playback: the cards' Stop buttons are about to go out of reach.
    void collapse();

    Page page() const { return page_; }
    // Shows History, Past Meetings or Walkthroughs. Leaving History stops playback.
    void setPage(Page page);
    // The page for a main-window tab (TabbedContentWidget's indexes).
    static Page pageForTab(int tab);
    // The panel of the Past Meetings or Walkthroughs page, kept at the sidebar's width and asked
    // to refresh() (an invokable slot) when its page shows; the sidebar takes it as a child.
    void setPageWidget(Page page, QWidget* widget);

    int sidebarWidth() const { return currentWidth_; }
    void setSidebarWidth(int width);

    // How many entry cards the list holds, each counted once.
    int renderedCardCount() const;
    // This computer's entry (or a host-kept one the list shows), looked up on the io executor;
    // done runs here, and never once the sidebar is gone.
    void entryFor(const QString& entryId, std::function<void(std::optional<HistoryEntry>)> done);

    // _apply_history_results: shows a finished load when generation is still the newest.
    void applyHistoryResults(std::uint64_t generation, const QString& query,
                             std::vector<HistoryEntry> entries, const QString& error,
                             const QString& hostNotice = {});
    // The newest load's generation.
    std::uint64_t loadGeneration() const { return gate_.current(); }
    // Starts a load of the list now (_load_history); the search box's text is the query.
    void loadHistory();

    // The header menu (Export history…, Open recordings folder, Clear history, Clear history +
    // recordings), parented to the sidebar, without showing it. The caller deletes it.
    QMenu* buildHeaderMenu();

    // The delete confirmation (_confirm_delete's box), built but not run. remote: the host-kept
    // entry being deleted there, else null; alsoOn: the host also keeping a copy; recordingSaved:
    // the entry's recording is still on disk, which offers to delete it too.
    struct DeleteConfirmation {
        std::unique_ptr<QMessageBox> box;
        QComboBox* audioChoice = nullptr;
        QCheckBox* dontAskAgain = nullptr;
    };
    DeleteConfirmation makeDeleteConfirmation(const HistoryEntry* remote, const QString& alsoOn,
                                              bool recordingSaved);

    // The card actions, connected to every card the list makes.
    void onEntryClicked(const QString& entryId);
    void onCopyRequested(const QString& entryId);
    void onCopyRawRequested(const QString& entryId);
    void onVersionRequested(const QString& entryId, const QString& version);
    void onDeleteRequested(const QString& entryId);
    // The entry dialog's Delete, confirmed there: deletes the entry and keeps its recording.
    void deleteEntryFromDialog(const HistoryEntry& entry);
    // The header menu's actions.
    void onClearHistory();
    void onClearHistoryAndRecordings();
    void onExportHistory();
    void onOpenRecordingsFolder();

    QWidget* contentWidget() const { return content_; }
    QPushButton* menuButton() const { return menuButton_; }
    QLabel* headerLabel() const { return headerLabel_; }
    CalendarGlyphButton* calendarButton() const { return calendarButton_; }
    QLineEdit* searchInput() const { return searchInput_; }
    QScrollArea* scrollArea() const { return scrollArea_; }
    QLabel* historyHeader() const { return historyHeader_; }
    QVBoxLayout* historyListLayout() const { return historyList_; }
    QPropertyAnimation* animation() const { return animation_; }
    const CardTable& historyCards() const { return cards_; }
    bool refreshPending() const { return refreshPending_; }

    // Runs a confirmation and returns the button pressed (QMessageBox::exec unless a test
    // answers for the user).
    std::function<int(QMessageBox&)> runMessageBox;
    // Puts text on the clipboard; false when that failed.
    std::function<bool(const QString& text)> copyText;
    // Opens a folder in the file manager (openFolderInFileManager unless a test replaces it).
    std::function<bool(const QString& folder)> openFolder;
    // Makes a card for an entry; the sidebar connects it.
    std::function<HistoryItemWidget*(const HistoryEntry& entry)> makeCard;

    // Whether a signal of this sidebar has a receiver, for "HistoryFeature connects every
    // signal" (architecture 8.5).
    bool hasReceivers(const QMetaMethod& signal) const { return isSignalConnected(signal); }

public Q_SLOTS:
    // Refreshes the shown page; while collapsed, the refresh waits for the next expand.
    void refresh();

Q_SIGNALS:
    void entrySelected(const QString& entryId);
    void entryCopied(const QString& entryId);
    void entryDeleted(const QString& entryId);
    void retranscribeRequested(const QString& audioPath);
    // The calendar button was clicked.
    void calendarRequested();
    // "Export history…" was chosen (the export dialog is M2's).
    void exportRequested();
    // Every animation frame's width, for the main window to resize in lockstep.
    void widthAnimated(int width);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void setupUi(const Clock& clock);
    void applyStyle();
    void onAnimationFinished();
    void onSearchTextChanged();
    void showHistoryMessage(const QString& message);
    QLabel* makeEmptyLabel(const QString& message) const;
    HistoryItemWidget* createCard(const HistoryEntry& entry);
    bool deleteConfirmationWanted() const;
    // _confirm_delete: whether to delete the recording too, or nullopt to keep the entry.
    std::optional<bool> confirmDelete(const HistoryEntry* entry, const QString& alsoOn,
                                      const std::optional<std::filesystem::path>& audioPath);
    void deleteEntry(const QString& entryId, bool deleteAudioFile);
    bool askToClear(const QString& title, const QString& text);
    // Runs work on the io executor and then, while the sidebar lives, done with its result.
    template <class Work, class Done> void inBackground(Work work, Done done);

    void stopVisiblePlayback();
    void deleteRemoteEntry(const QString& entryId);
    void addRemoteNotice(const QString& notice);

    // One load's request to the host, answered on another io task (_start_remote_listing).
    struct RemoteListing {
        history::RemoteRecordSource* records = nullptr;
        std::vector<HistoryEntry> entries;
        std::int64_t limit = 0;
        std::mutex mutex;
        std::condition_variable arrived;
        bool done = false;
        std::vector<std::string> copies;
        std::vector<Json> items;
        std::string notice;
        // Set once the load showed this computer's entries without the host's.
        std::function<void()> late;
        std::pair<std::vector<HistoryEntry>, std::string> merged();
    };
    static std::shared_ptr<RemoteListing>
    startRemoteListing(history::RemoteRecordSource* records, Executor& io, const std::string& query,
                       std::int64_t limit, const std::vector<HistoryEntry>& entries);

    history::RemoteRecordSource* remoteRecords_ = nullptr;
    remote::RecordSyncControl* remoteActions_ = nullptr;
    std::shared_ptr<HistoryStore> store_;
    Executor& io_;
    SettingsStore* settings_;
    std::shared_ptr<qt::QtUiExecutor> ui_;
    qt::TokenGate gate_;
    std::shared_ptr<std::atomic<bool>> loadCanceled_;
    std::chrono::milliseconds remoteMergeWait_ = kRemoteMergeWait;

    bool expanded_ = false;
    int currentWidth_ = kCollapsedWidth;
    bool refreshPending_ = true;
    Page page_ = Page::History;
    std::map<Page, QPointer<QWidget>> pageWidgets_;
    CardTable cards_;
    // Entries kept on the paired host, and the host also keeping one of ours, by id (M9 fills
    // them through the host's listing).
    std::map<std::string, HistoryEntry> remoteEntries_;
    std::map<std::string, std::string> alsoOn_;

    QWidget* content_ = nullptr;
    QPushButton* menuButton_ = nullptr;
    QLabel* headerLabel_ = nullptr;
    CalendarGlyphButton* calendarButton_ = nullptr;
    QLineEdit* searchInput_ = nullptr;
    QTimer* searchTimer_ = nullptr;
    QScrollArea* scrollArea_ = nullptr;
    QLabel* historyHeader_ = nullptr;
    QVBoxLayout* historyList_ = nullptr;
    QPropertyAnimation* animation_ = nullptr;
};

} // namespace fv
