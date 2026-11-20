#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QString>
#include <QWidget>

#include "appkit/qt_bridge.h"
#include "base/clock.h"
#include "base/executor.h"
#include "base/json.h"
#include "ui_widgets/list_reconcile.h"

class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace fv {

class CalendarGlyphButton;
class MeetingDeleteDialog;
class PastMeetingItem;
class SettingsStore;

// What the panel reads and how it behaves; everything is optional.
struct PastMeetingsOptions {
    // Holds confirm_meeting_delete (asked when null, as when the setting cannot be read).
    SettingsStore* settings = nullptr;
    // The zone dates are shown in; system_clock() when null. Must outlive the panel.
    const Clock* clock = nullptr;
    // Whether the provider applies the query itself, as the repository does. When false the panel
    // filters what it was given (the seam Python's explicit meeting providers used).
    bool sourceFiltered = true;
    // Gives a row without a content_summary one, on io (summarize_meeting_content).
    std::function<Json(const std::string& meetingId)> summarize;
    // The paired host's meetings (_start_remote_listing), asked on io after this computer's:
    // hostWanted says whether to ask (listing_wanted), hostMerge blocks on the host and returns
    // `local` with the host's meetings merged in. A load shows the local rows when the host has
    // not answered within remoteMergeWait, then the merged list when it does. Either empty: no
    // host is asked.
    std::function<bool()> hostWanted;
    std::function<std::vector<Json>(std::vector<Json> local, const QString& query, int limit)>
        hostMerge;
    // REMOTE_MERGE_WAIT_S.
    std::chrono::milliseconds remoteMergeWait{400};
};

// The Past Meetings page of the History sidebar (ui_qt/widgets/past_meetings_panel.py
// PastMeetingsPanel, objectName pastMeetingsContent): a header with its menu and the calendar
// button, a search box, a count line and the newest 100 meetings as cards. Live meetings (active,
// paused, ending) are not listed.
//
// Rows come from the provider, which runs on `io` and never on the GUI thread; a result is shown
// only while it is the newest request's (a token), so a load that ends after the panel is gone
// or after the user typed again is dropped. The provider gets one row more than is shown, to
// know there is more, and the search text (stripped, lower case).
//
// Intent leaves through signals. Where it differs from Python, which opens the export dialog,
// the meetings folder and the host's downloads itself: export, folder and a host-kept meeting
// are asked for by exportRequested, openMeetingsFolderRequested and hostFetchRequested, and the
// owner answers (exportableMeetings, openMeetingsFolder, onHostFetchProgress and
// onHostFetchDone). The slots refresh and setSelectedMeetingId are invoked by name.
// Affinity: GUI thread.
class PastMeetingsPanel : public QWidget {
    Q_OBJECT
public:
    using Provider = std::function<std::vector<Json>(int limit, const QString& query)>;

    // Cards shown at most; a search still reaches every meeting.
    static constexpr int kMaxMeetings = 100;
    // Typing restarts the search this long after the last key.
    static constexpr int kSearchDelayMs = 250;

    // provider and io must outlive the panel's loads (the panel copies the provider).
    PastMeetingsPanel(Provider provider, Executor& io, const PastMeetingsOptions& options = {},
                      QWidget* parent = nullptr);
    ~PastMeetingsPanel() override;

    // The rows shown after the last load, live ones removed, in display order before the cap.
    const std::vector<Json>& meetings() const { return meetings_; }
    // The meetings an export offers: those kept on this computer.
    std::vector<Json> exportableMeetings() const;
    std::string pairedHostFingerprint(const QString& meetingId) const;
    const QString& selectedMeetingId() const { return selectedId_; }
    void setSpeakerUploadAvailable(bool available) { speakerUploadAvailable_ = available; }

    // The newest load's generation.
    std::uint64_t loadGeneration() const { return gate_.current(); }
    // _apply_meeting_results: shows a finished load when generation is still the newest.
    void applyMeetingResults(std::uint64_t generation, std::vector<Json> meetings,
                             const QString& error, bool sourceFiltered);
    // _rebuild_list: the cards, count line and placeholders for the rows held and the search text.
    void rebuildList();

    // The header menu (Refresh, Export past meetings…, Open meetings folder, Clear history, Clear
    // history + recordings), parented to the panel, without showing it. The caller deletes it.
    QMenu* buildHeaderMenu();

    // _confirm_delete: the delete confirmation, then deleteMeetingRequested.
    void confirmDelete(const QString& meetingId);
    // The header menu's clears, each after its question.
    void onClearHistory();
    void onClearHistoryAndRecordings();

    // A host fetch's progress text on the card, and its end ("then" is "open" or "delete").
    void onHostFetchProgress(const QString& meetingId, const QString& text);
    void onHostFetchDone(const QString& meetingId, const QString& then, const QString& error);

    // The parts tests and the sidebar reach.
    QPushButton* menuButton() const { return menuButton_; }
    CalendarGlyphButton* calendarButton() const { return calendarButton_; }
    QLineEdit* searchInput() const { return searchInput_; }
    QScrollArea* scrollArea() const { return scrollArea_; }
    QLabel* sectionHeader() const { return sectionHeader_; }
    QVBoxLayout* meetingsListLayout() const { return meetingsList_; }
    const CardTable& meetingCards() const { return cards_; }
    // The cards in display order.
    std::vector<PastMeetingItem*> cards() const;

    // How the panel asks: MeetingDeleteDialog::exec and QMessageBox::question unless a test
    // answers for the user. The dialog runner returns the dialog's result.
    std::function<int(MeetingDeleteDialog&)> runDeleteDialog;
    std::function<bool(const QString& title, const QString& text)> confirm;
    // What clearing also deletes on the paired host, appended to the clear questions ("" when
    // nothing is kept there): M9's.
    std::function<QString()> hostClearNote;

public Q_SLOTS:
    // Reloads the persisted meetings and rebuilds the card list.
    void refresh();
    // Highlights the card that matches the leftover card, if listed ("" clears it).
    void setSelectedMeetingId(const QString& meetingId);

Q_SIGNALS:
    void meetingSelected(const QString& meetingId);
    void copyTranscriptRequested(const QString& meetingId);
    void speakerUploadRequested(const QString& meetingId);
    // (meeting id, delete its recordings too).
    void deleteMeetingRequested(const QString& meetingId, bool deleteRecordings);
    // (delete the recordings too).
    void clearMeetingsRequested(bool deleteRecordings);
    void calendarRequested();
    // "Export past meetings…" was chosen (the dialog is the owner's: exportableMeetings).
    void exportRequested();
    // "Open meetings folder" was chosen (openMeetingsFolder).
    void openMeetingsFolderRequested();
    // A meeting kept on a paired host was opened or deleted there: (id, host, "open" or
    // "delete"). The owner downloads or deletes it and answers with onHostFetchDone.
    void hostCopyDeleteRequested(const QString& meetingId, const QString& fingerprint,
                                 const QString& revision);
    void hostFetchRequested(const QString& meetingId, const QString& host, const QString& then);

private:
    void setupUi();
    void clearList();
    void applySelection();
    void showLoadFailure();
    QLabel* placeholder(const QString& message) const;
    PastMeetingItem* createCard(const Json& meeting);
    void onCardSelected(const QString& meetingId);
    void fetchFromHost(const QString& meetingId, const QString& host, const QString& then);
    const Json* meetingById(const QString& meetingId) const;
    PastMeetingItem* cardFor(const QString& meetingId) const;
    bool deleteConfirmationWanted() const;

    std::shared_ptr<Provider> provider_;
    Executor& io_;
    PastMeetingsOptions options_;
    const Clock* clock_;
    std::shared_ptr<qt::QtUiExecutor> ui_;
    qt::TokenGate gate_;
    std::shared_ptr<std::atomic<bool>> loadCanceled_;

    QString selectedId_;
    bool speakerUploadAvailable_ = false;
    std::vector<Json> meetings_;
    bool sourceFiltered_;
    bool hasMore_ = false;
    CardTable cards_;
    std::vector<QString> fetching_;

    QPushButton* menuButton_ = nullptr;
    CalendarGlyphButton* calendarButton_ = nullptr;
    QLineEdit* searchInput_ = nullptr;
    QTimer* searchTimer_ = nullptr;
    QScrollArea* scrollArea_ = nullptr;
    QLabel* sectionHeader_ = nullptr;
    QVBoxLayout* meetingsList_ = nullptr;
};

// "Open meetings folder": the folder, made when missing, shown in the file manager. False, with
// the reason logged and nothing opened, when the folder cannot be made.
bool openMeetingsFolder(const std::filesystem::path& folder);

} // namespace fv
