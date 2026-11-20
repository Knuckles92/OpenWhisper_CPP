#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <QDialog>
#include <QList>
#include <QPointer>
#include <QString>

#include "history/calendar.h"
#include "history/calendar_source.h"
#include "history/entry.h"

class QComboBox;
class QFrame;
class QLabel;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;
class QWidget;

namespace fv {

class AgendaCard;
class Clock;
class Executor;
class KindChip;
class MonthGrid;
class MonthPicker;
class MonthTitleButton;
class YearStrip;

namespace history_calendar_detail {
class CalendarRelay;
}

// What the History calendar reads (services/history_calendar.CalendarSource, which Python's
// tests replace with a fake of these five calls). Every call blocks and runs on the io
// executor, never on the GUI thread.
// Affinity: thread-safe.
class CalendarReads {
public:
    virtual ~CalendarReads() = default;
    virtual std::vector<history::Mark> load_index() = 0;
    virtual bool remote_wanted() = 0;
    virtual std::vector<history::Mark> load_remote(const std::vector<std::string>& local_ids) = 0;
    virtual std::vector<history::CalendarItem> load_month(int year, int month) = 0;
    // Why the host's records could not be listed, or "".
    virtual std::string notice() = 0;
};

// The reads of a CalendarSource, one call at a time; source must outlive every read.
std::shared_ptr<CalendarReads> calendarReadsOf(history::CalendarSource& source);
// The same, keeping the source alive while any read holds it.
std::shared_ptr<CalendarReads> calendarReadsOf(std::shared_ptr<history::CalendarSource> source);

// The History calendar (ui_qt/dialogs/history_calendar_dialog.py HistoryCalendarDialog): every
// dictation, transcribed file and meeting by month, on the day it happened. Each day is tinted
// by how much was recorded and lists what it holds, coloured by kind; the agenda beside it
// shows the selected day in full. The kind chips double as the legend and a filter, and a
// Where filter appears once records are kept on more than one computer.
//
// Reads run on io and come back through a relay that lives for the session, so a read never
// holds or emits on a dialog: the index first (it gives each day its heat), then the shown
// month, then the paired host's records, then the months either side (kPrefetch). A result of
// an older generation is dropped. Opening a record is left to the main window: entryRequested
// carries the History entry, meetingRequested the meeting id.
// Affinity: GUI thread.
class HistoryCalendarDialog : public QDialog {
    Q_OBJECT
public:
    // Months read ahead on each side of the one shown.
    static constexpr int kPrefetch = 1;
    // Merges a burst of refreshIfVisible calls.
    static constexpr int kRefreshDelayMs = 60;
    // Reopened, today pulses after this long.
    static constexpr int kPulseDelayMs = 220;

    // Downloads a host-kept meeting here (record_sync.check_out("meeting", id, progress)),
    // calling progress(got, total) as bytes arrive; throws on failure. Runs on io.
    using MeetingCheckOut = std::function<void(
        std::string_view kind, std::string_view id,
        const std::function<void(std::int64_t got, std::int64_t total)>& progress)>;

    // source must outlive the dialog's reads; today comes from the system clock and the week
    // starts on the locale's first day.
    HistoryCalendarDialog(history::CalendarSource& source, Executor& io, QWidget* parent = nullptr);
    // clock gives today (in its zone) and must outlive the dialog; firstWeekday is Python's
    // (0 is Monday), the locale's when not given.
    HistoryCalendarDialog(std::shared_ptr<CalendarReads> reads, Executor& io, const Clock& clock,
                          std::optional<int> firstWeekday = std::nullopt,
                          QWidget* parent = nullptr);
    ~HistoryCalendarDialog() override;

    // Reads everything again: after a recording, a delete or an edit elsewhere.
    void refresh();
    // Refreshes soon when shown (bursts merge), or the next time it shows.
    void refreshIfVisible();
    // Today, in its month, with the pulse that shows where it is (go_today).
    void showToday();
    // Goes to a month, selecting day when it is in that month, else the month's default day.
    void showMonth(history::MonthKey key, std::optional<history::Day> day = std::nullopt);
    void stepMonth(int step);
    void selectDay(history::Day day);
    // The month and year picker, under the month's name.
    void openMonthPicker();
    // Enter or a double click: opens the day's first record.
    void activateDay(history::Day day);
    // Opens or copies a record of the shown month (an agenda card's click and menu).
    void openItem(const history::CalendarItem& item);
    void copyItem(const history::CalendarItem& item);
    // Drives every running animation (the grid's, the year strip's, the cards' fade) to its end.
    void finishAnimations();

    // Puts text on the clipboard; false when that failed.
    std::function<bool(const QString& text)> copyText;
    // Fetches host-kept meetings; without one such a meeting does not open (M9's record sync
    // provides it).
    void setMeetingCheckOut(MeetingCheckOut checkOut);
    void setPairedMeetingCheckOut(MeetingCheckOut checkOut);

    history::MonthKey view() const { return view_; }
    history::Day selected() const { return selected_; }
    const std::optional<history::KindSet>& kinds() const { return kinds_; }
    const std::optional<history::Place>& place() const { return place_; }
    const history::CalendarIndex& index() const { return index_; }
    bool isLoaded() const { return loaded_; }
    bool isStale() const { return stale_; }
    const QList<AgendaCard*>& cards() const { return cards_; }
    AgendaCard* card(std::string_view recordId) const;

    MonthGrid* grid() const { return grid_; }
    MonthTitleButton* monthTitle() const { return monthTitle_; }
    MonthPicker* monthPicker() const { return monthPicker_; }
    YearStrip* yearStrip() const { return yearStrip_; }
    QPushButton* todayButton() const { return todayButton_; }
    QPushButton* prevButton() const { return prevButton_; }
    QPushButton* nextButton() const { return nextButton_; }
    QLabel* streakLabel() const { return streakLabel_; }
    KindChip* kindChip(std::string_view kind) const;
    QComboBox* placeCombo() const { return placeCombo_; }
    QLabel* statsLabel() const { return statsLabel_; }
    QPushButton* busiestButton() const { return busiestButton_; }
    QLabel* noticeLabel() const { return noticeLabel_; }
    QFrame* agenda() const { return agenda_; }
    QLabel* agendaWeekday() const { return agendaWeekday_; }
    QLabel* todayBadge() const { return todayBadge_; }
    QLabel* agendaDate() const { return agendaDate_; }
    QLabel* agendaSummary() const { return agendaSummary_; }
    QScrollArea* agendaScroll() const { return agendaScroll_; }
    QWidget* agendaList() const { return agendaList_; }
    QLabel* hintLabel() const { return hintLabel_; }
    QTimer* refreshTimer() const { return refreshTimer_; }
    QTimer* pulseTimer() const { return pulseTimer_; }

Q_SIGNALS:
    void entryRequested(const fv::HistoryEntry& entry);
    void meetingRequested(const QString& meetingId);
    void meetingCopyRequested(const QString& meetingId);
    void entryCopied(const QString& entryId);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void setupUi();
    QFrame* buildAgenda();
    void fitToScreen(int width, int height);
    void run(std::function<void()> work);
    void requestMonths(history::MonthKey center);
    void onIndexLoaded(std::uint64_t generation,
                       const std::shared_ptr<const std::vector<history::Mark>>& marks,
                       const QString& notice);
    void onMonthLoaded(std::uint64_t generation, int year, int month,
                       const std::shared_ptr<const std::vector<history::CalendarItem>>& items);
    void onKindToggled(KindChip* chip);
    void syncPlaceFilter();
    void onPlaceChanged(int index);
    bool visible(const history::CalendarItem& item) const;
    void onMonthPicked(int year, int month);
    history::Day defaultDay(history::MonthKey key) const;
    void showBusiest();
    std::map<history::Day, std::vector<history::CalendarItem>> itemsByDay() const;
    std::vector<history::CalendarItem> dayItems(history::Day day) const;
    void render(int direction);
    void renderDetails();
    void renderHeader();
    std::vector<history::MonthKey> stripMonths() const;
    void clearAgenda();
    void renderAgenda(bool animate = true);
    // A card's click or menu choice: opens or copies a copy of its record. Opening may run a
    // modal dialog (the entry dialog's exec), and a refresh in its nested event loop would
    // otherwise delete the card under its own event handler and the record the open still
    // reads, so the cards cleared meanwhile are only hidden until the open returns.
    void actOnCard(const AgendaCard& card, bool copy);
    QWidget* emptyStateFor(history::Day day);
    bool filtered() const;
    void fetchMeeting(const history::CalendarItem& item);
    void onFetchProgress(std::uint64_t token, const QString& meetingId, const QString& text);
    void onFetchDone(std::uint64_t token, const QString& meetingId, const QString& error);
    history::Day today() const;

    std::shared_ptr<CalendarReads> reads_;
    Executor& io_;
    const Clock& clock_;
    int firstWeekday_;
    history_calendar_detail::CalendarRelay* relay_;
    history::CalendarIndex index_;
    bool loaded_ = false;
    bool stale_ = true;
    std::uint64_t generation_;
    // Rows read per month; fresh_ holds those read since the last refresh.
    std::map<history::MonthKey, std::vector<history::CalendarItem>> months_;
    std::set<history::MonthKey> fresh_;
    std::set<history::MonthKey> loading_;
    history::MonthKey view_;
    history::Day selected_;
    std::optional<history::Day> agendaDay_;
    std::optional<history::KindSet> kinds_;
    std::optional<history::Place> place_;
    std::string notice_;
    QList<AgendaCard*> cards_;
    // Opens from a card under way, and the agenda widgets cleared while one was.
    int acting_ = 0;
    QList<QPointer<QWidget>> retired_;
    std::set<std::string> fetching_;
    // Picks this dialog's downloads off the shared relay; reads go by generation.
    std::uint64_t token_;
    bool pulsePending_ = true;
    int scrollTo_ = 0;
    MeetingCheckOut checkOut_;
    MeetingCheckOut pairedCheckOut_;

    QTimer* refreshTimer_ = nullptr;
    QTimer* pulseTimer_ = nullptr;
    QTimer* scrollTimer_ = nullptr;
    QPushButton* todayButton_ = nullptr;
    QPushButton* prevButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    MonthTitleButton* monthTitle_ = nullptr;
    MonthPicker* monthPicker_ = nullptr;
    QLabel* streakLabel_ = nullptr;
    std::map<std::string, KindChip*, std::less<>> kindChips_;
    QComboBox* placeCombo_ = nullptr;
    QLabel* statsLabel_ = nullptr;
    QPushButton* busiestButton_ = nullptr;
    QLabel* noticeLabel_ = nullptr;
    MonthGrid* grid_ = nullptr;
    YearStrip* yearStrip_ = nullptr;
    QFrame* agenda_ = nullptr;
    QLabel* agendaWeekday_ = nullptr;
    QLabel* todayBadge_ = nullptr;
    QLabel* agendaDate_ = nullptr;
    QLabel* agendaSummary_ = nullptr;
    QScrollArea* agendaScroll_ = nullptr;
    QWidget* agendaList_ = nullptr;
    QVBoxLayout* agendaLayout_ = nullptr;
    QLabel* hintLabel_ = nullptr;
};

} // namespace fv
