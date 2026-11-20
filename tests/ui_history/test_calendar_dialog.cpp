// The History calendar window: navigation, filters, the day agenda, and opening records
// (OpenWhisper's tests/test_history_calendar_dialog.py). Reads run at once on the test's thread,
// as Python's threaded=False does, except in the closed-mid-read test, which uses real threads.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTest>
#include <QTimer>

#include "base/executor.h"
#include "base/testing/fake_clock.h"
#include "calendar_test_support.h"
#include "ui_history/history_calendar.h"
#include "ui_history/history_calendar_dialog.h"

using fv::AgendaCard;
using fv::HistoryCalendarDialog;
using fv::MonthGrid;
using fv::MonthPicker;
using fv::YearStrip;
using fvtest::at;
using fvtest::dayOf;
using fvtest::FakeReads;
using fvtest::item;
namespace hc = fv::history;

namespace {

const hc::Day kToday = dayOf(2026, 9, 27);

// The `dialog` fixture: today pinned, weeks from Sunday, reads at once, shown at 1180 x 800.
struct Calendar {
    explicit Calendar(std::vector<hc::CalendarItem> items = fvtest::september(),
                      std::vector<hc::CalendarItem> remote = {})
        : clock(fvtest::todayNoon()),
          source(std::make_shared<FakeReads>(std::move(items), std::move(remote))),
          window(source, io, clock, hc::SUNDAY) {
        window.resize(1180, 800);
        window.show();
        QCoreApplication::processEvents();
    }

    fv::FakeClock clock;
    fvtest::InlineExecutor io;
    std::shared_ptr<FakeReads> source;
    HistoryCalendarDialog window;
};

std::vector<std::string> cardIds(const HistoryCalendarDialog& window) {
    std::vector<std::string> ids;
    for (const AgendaCard* card : window.cards()) {
        ids.push_back(card->item().record_id);
    }
    return ids;
}

using Ids = std::vector<std::string>;

hc::MonthKey monthOf(const MonthGrid& grid) {
    return grid.month();
}

} // namespace

TEST_CASE("opens on today with its recordings", "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::test_opens_on_today_with_its_recordings
    Calendar calendar;
    HistoryCalendarDialog& window = calendar.window;
    CHECK(monthOf(*window.grid()) == hc::MonthKey{2026, 9});
    CHECK(window.monthTitle()->text() == QStringLiteral("September 2026"));
    CHECK(window.grid()->selected() == kToday);
    CHECK(cardIds(window) == Ids{"d1", "d2", "m1"});
    CHECK(window.todayBadge()->isVisibleTo(&window));
    CHECK(window.agendaSummary()->text().contains(QStringLiteral("3 recordings")));
    CHECK(window.agendaSummary()->text().contains(QStringLiteral(u"2 dictations · 1 meeting")));
    const QString stats = window.statsLabel()->text();
    CHECK(stats.contains(QStringLiteral("<b>5</b> recordings")));
    CHECK(stats.contains(QStringLiteral("<b>3</b> days")));
    CHECK(window.kindChip(hc::DICTATION_KIND)->text() == QStringLiteral("Dictations  3"));
    CHECK(window.kindChip(hc::MEETING_KIND)->text() == QStringLiteral("Meetings  1"));
    // The 26th and 27th in a row.
    CHECK(window.streakLabel()->isVisibleTo(&window));
    CHECK(window.streakLabel()->text() == QStringLiteral("2-day streak"));
    CHECK(window.busiestButton()->text() == QStringLiteral(u"Busiest: Sun, Sep 27 · 3"));
}

TEST_CASE("months are read ahead once", "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::test_months_are_read_ahead_once
    Calendar calendar;
    std::vector<hc::MonthKey> reads = calendar.source->monthReads();
    std::set<hc::MonthKey> distinct(reads.begin(), reads.end());
    CHECK(distinct == std::set<hc::MonthKey>{{2026, 8}, {2026, 9}, {2026, 10}});
    const std::size_t before = reads.size();
    calendar.window.stepMonth(-1);
    QCoreApplication::processEvents();
    reads = calendar.source->monthReads();
    // August was read ahead; only July is new.
    CHECK(std::vector<hc::MonthKey>(reads.begin() + static_cast<std::ptrdiff_t>(before),
                                    reads.end()) == std::vector<hc::MonthKey>{{2026, 7}});
}

TEST_CASE("selecting days and months", "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::test_selecting_days_and_months
    Calendar calendar;
    HistoryCalendarDialog& window = calendar.window;
    window.selectDay(dayOf(2026, 9, 26));
    CHECK(cardIds(window) == Ids{"f1"});
    CHECK(window.agendaWeekday()->text() == QStringLiteral("Saturday"));
    window.selectDay(dayOf(2026, 9, 28));
    CHECK(cardIds(window).empty());

    // A day in another month goes there.
    window.selectDay(dayOf(2026, 5, 4));
    CHECK(monthOf(*window.grid()) == hc::MonthKey{2026, 5});
    CHECK(cardIds(window) == Ids{"d4"});
    CHECK(window.agendaDate()->text() == QStringLiteral("May 4"));
    CHECK_FALSE(window.streakLabel()->isVisibleTo(&window));

    window.showToday();
    CHECK(monthOf(*window.grid()) == hc::MonthKey{2026, 9});
    CHECK(window.grid()->selected() == kToday);
}

TEST_CASE("a new month selects its latest active day", "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::test_a_new_month_selects_its_latest_active_day
    Calendar calendar;
    calendar.window.showMonth({2026, 5});
    CHECK(calendar.window.grid()->selected() == dayOf(2026, 5, 4));
    calendar.window.showMonth({2026, 6});
    CHECK(calendar.window.grid()->selected() == dayOf(2026, 6, 1));
}

TEST_CASE("an empty month offers the closest one", "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::test_an_empty_month_offers_the_closest_one
    Calendar calendar;
    HistoryCalendarDialog& window = calendar.window;
    window.showMonth({2026, 7});
    // A layout shows new children of a visible parent on the next event loop pass.
    QCoreApplication::processEvents();
    CHECK(window.statsLabel()->text().contains(QStringLiteral("Nothing recorded in July")));
    std::vector<QPushButton*> jump;
    for (QPushButton* button : window.agendaList()->findChildren<QPushButton*>()) {
        if (button->objectName() == QStringLiteral("calendarJumpBtn") &&
            button->isVisibleTo(&window)) {
            jump.push_back(button);
        }
    }
    REQUIRE(jump.size() == 1);
    CHECK(jump[0]->text() == QStringLiteral(u"← Go to May 2026"));
    jump[0]->click();
    CHECK(monthOf(*window.grid()) == hc::MonthKey{2026, 5});
}

TEST_CASE("nothing recorded yet", "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::test_nothing_recorded_yet
    Calendar calendar({});
    QStringList labels;
    for (const QLabel* label : calendar.window.agendaList()->findChildren<QLabel*>()) {
        labels.append(label->text());
    }
    CHECK(labels.contains(QStringLiteral("Nothing recorded yet")));
}

TEST_CASE("kind chips filter and one always stays on", "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::test_kind_chips_filter_and_one_always_stays_on
    Calendar calendar;
    HistoryCalendarDialog& window = calendar.window;
    window.kindChip(hc::MEETING_KIND)->setChecked(false);
    CHECK(cardIds(window) == Ids{"d1", "d2"});
    CHECK(window.grid()->details().days.at(kToday).count == 2);
    window.kindChip(hc::DICTATION_KIND)->setChecked(false);
    window.selectDay(dayOf(2026, 9, 26));
    CHECK(cardIds(window) == Ids{"f1"});
    window.kindChip(hc::FILE_KIND)->setChecked(false);
    CHECK(window.kindChip(hc::FILE_KIND)->isChecked());
    REQUIRE(window.kinds().has_value());
    CHECK(*window.kinds() == hc::KindSet{std::string(hc::FILE_KIND)});
}

TEST_CASE("where filter appears with more than one place",
          "[test_history_calendar_dialog][widget]") {
    // from
    // tests/test_history_calendar_dialog.py::test_where_filter_appears_with_more_than_one_place
    {
        Calendar calendar;
        CHECK_FALSE(calendar.window.placeCombo()->isVisibleTo(&calendar.window));
    }
    const hc::CalendarItem hosted =
        item("h1", at(2026, 9, 27, 18, 0), {.place = hc::Place{std::string(hc::ON), "jed"}});
    Calendar calendar(fvtest::september(), {hosted});
    HistoryCalendarDialog& window = calendar.window;
    CHECK(window.placeCombo()->isVisibleTo(&window));
    QStringList labels;
    for (int row = 0; row < window.placeCombo()->count(); ++row) {
        labels.append(window.placeCombo()->itemText(row));
    }
    CHECK(labels == QStringList{QStringLiteral("Everywhere"), QStringLiteral("This computer"),
                                QStringLiteral("Kept on jed")});
    CHECK(cardIds(window) == Ids{"d1", "d2", "m1", "h1"});
    window.placeCombo()->setCurrentIndex(2);
    CHECK(cardIds(window) == Ids{"h1"});
    const auto* chip =
        window.cards().front()->findChild<QLabel*>(QStringLiteral("calendarPlaceChip"));
    REQUIRE(chip != nullptr);
    CHECK(chip->text() == QStringLiteral("On jed"));
}

TEST_CASE("opening and copying records", "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::test_opening_and_copying_records
    Calendar calendar;
    HistoryCalendarDialog& window = calendar.window;
    std::vector<std::string> opened;
    QStringList meetings;
    QStringList copies;
    QObject::connect(&window, &HistoryCalendarDialog::entryRequested,
                     [&opened](const fv::HistoryEntry& entry) { opened.push_back(entry.id); });
    QObject::connect(&window, &HistoryCalendarDialog::meetingRequested,
                     [&meetings](const QString& id) { meetings.append(id); });
    QObject::connect(&window, &HistoryCalendarDialog::meetingCopyRequested,
                     [&copies](const QString& id) { copies.append(id); });

    QTest::mouseClick(window.card("d1"), Qt::LeftButton);
    CHECK(opened == Ids{"d1"});
    window.card("m1")->open();
    CHECK(meetings == QStringList{QStringLiteral("m1")});
    window.card("m1")->copy();
    CHECK(copies == QStringList{QStringLiteral("m1")});

    // The clipboard seam stands for QApplication.clipboard(): no test writes the real one.
    QString clipboard;
    window.copyText = [&clipboard](const QString& text) {
        clipboard = text;
        return true;
    };
    QStringList copied;
    QObject::connect(&window, &HistoryCalendarDialog::entryCopied,
                     [&copied](const QString& id) { copied.append(id); });
    window.card("d2")->copy();
    CHECK(clipboard == QStringLiteral("Second thought"));
    CHECK(copied == QStringList{QStringLiteral("d2")});

    // Enter (or a double click) opens the day's first recording.
    window.activateDay(kToday);
    CHECK(opened == Ids{"d1", "d1"});
}

TEST_CASE("a record opened from its card outlives a refresh while it is open", "[widget]") {
    // The entry dialog runs modally over the calendar, and an edit in it refreshes the calendar
    // inside that nested event loop: the agenda is rebuilt, and the card that was clicked, still
    // inside its own press handler, and the record being opened must not go with it.
    Calendar calendar;
    HistoryCalendarDialog& window = calendar.window;
    QPointer<AgendaCard> clicked(window.card("d1"));
    REQUIRE_FALSE(clicked.isNull());
    bool aliveAfterRefresh = false;
    std::string opened;
    QObject::connect(&window, &HistoryCalendarDialog::entryRequested,
                     [&](const fv::HistoryEntry& entry) {
                         QEventLoop modal;
                         QTimer::singleShot(0, &window, [&window] { window.refresh(); });
                         QTimer::singleShot(50, &modal, &QEventLoop::quit);
                         modal.exec();
                         aliveAfterRefresh = !clicked.isNull();
                         opened = entry.id;
                     });
    QTest::mouseClick(clicked.data(), Qt::LeftButton);
    CHECK(aliveAfterRefresh);
    CHECK(opened == "d1");
    CHECK(cardIds(window) == Ids{"d1", "d2", "m1"});
    CHECK(window.card("d1") != clicked.data());
    // Once the open returned, the old card goes with the next pass of the event loop.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(clicked.isNull());

    // Copying from a card's menu goes the same way.
    QPointer<AgendaCard> copiedFrom(window.card("d2"));
    window.copyText = [&window](const QString&) {
        window.refresh();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return true;
    };
    QStringList copied;
    QObject::connect(&window, &HistoryCalendarDialog::entryCopied,
                     [&copied](const QString& id) { copied.append(id); });
    copiedFrom->copy();
    CHECK(copied == QStringList{QStringLiteral("d2")});
}

TEST_CASE("a host-kept meeting is fetched through the check-out, then opened", "[widget]") {
    // M9's record sync supplies the check-out; here a fake stands in for it.
    const hc::CalendarItem hosted = item("mh", at(2026, 9, 27, 19, 0),
                                         {.kind = std::string(hc::MEETING_KIND),
                                          .place = hc::Place{std::string(hc::ON), "jed"},
                                          .title = "Remote sync"});
    Calendar calendar(fvtest::september(), {hosted});
    HistoryCalendarDialog& window = calendar.window;
    QStringList meetings;
    QObject::connect(&window, &HistoryCalendarDialog::meetingRequested,
                     [&meetings](const QString& id) { meetings.append(id); });

    // Without a record sync, a host-kept meeting does not open.
    window.card("mh")->open();
    CHECK(meetings.isEmpty());

    std::vector<std::pair<std::string, std::string>> calls;
    QStringList progress;
    window.setMeetingCheckOut([&](std::string_view kind, std::string_view id, const auto& report) {
        calls.emplace_back(std::string(kind), std::string(id));
        report(50, 100);
        if (const AgendaCard* card = window.card("mh")) {
            progress.append(card->progressLabel()->text());
        }
    });
    window.card("mh")->open();
    CHECK(calls == std::vector<std::pair<std::string, std::string>>{{"meeting", "mh"}});
    CHECK(progress == QStringList{QStringLiteral(u"Getting it from jed… 50%")});
    CHECK(meetings == QStringList{QStringLiteral("mh")});

    // A failed download says why on its card and opens nothing.
    window.setMeetingCheckOut([](std::string_view, std::string_view, const auto&) {
        throw std::runtime_error("the host went away");
    });
    window.card("mh")->open();
    CHECK(meetings.size() == 1);
    REQUIRE(window.card("mh") != nullptr);
    CHECK(window.card("mh")->progressLabel()->text() ==
          QStringLiteral("Couldn't open it: the host went away"));
}

namespace {

// Blocks the threads that wait on it until opened.
class Gate {
public:
    void wait() {
        std::unique_lock lock(mutex_);
        opened_.wait_for(lock, std::chrono::seconds(5), [this] { return open_; });
    }
    void open() {
        {
            const std::lock_guard lock(mutex_);
            open_ = true;
        }
        opened_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable opened_;
    bool open_ = false;
};

class SlowReads final : public FakeReads {
public:
    SlowReads(Gate& gate, std::atomic<int>& done)
        : FakeReads(fvtest::september()), gate_(gate), done_(done) {}
    std::vector<hc::Mark> load_index() override {
        gate_.wait();
        return FakeReads::load_index();
    }
    std::vector<hc::CalendarItem> load_month(int year, int month) override {
        auto items = FakeReads::load_month(year, month);
        if (year == 2026 && month == 10) {
            // The last read of a refresh.
            ++done_;
        }
        return items;
    }

private:
    Gate& gate_;
    std::atomic<int>& done_;
};

} // namespace

TEST_CASE("a calendar closed mid-read is not held by its workers",
          "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::
    //     test_a_calendar_closed_mid_read_is_not_held_by_its_workers
    // The reads and the download used to run as the dialog's own methods: the worker kept a
    // closed dialog alive, then dropped the last reference or emitted on it from its own thread.
    Gate gate;
    std::atomic<int> done{0};
    fv::ThreadPool pool("calendar-test", 2);
    fv::FakeClock clock(fvtest::todayNoon());
    auto source = std::make_shared<SlowReads>(gate, done);
    auto* window = new HistoryCalendarDialog(source, pool, clock, hc::SUNDAY);
    window->setMeetingCheckOut(
        [&gate, &done](std::string_view, std::string_view, const auto& progress) {
            gate.wait();
            progress(50, 100);
            ++done;
        });
    const hc::CalendarItem hosted = item("mh", at(2026, 9, 27, 19, 0),
                                         {.kind = std::string(hc::MEETING_KIND),
                                          .place = hc::Place{std::string(hc::ON), "jed"},
                                          .title = "Remote sync"});
    window->refresh();
    window->openItem(hosted);
    QPointer<HistoryCalendarDialog> ref(window);
    window->deleteLater();
    window = nullptr;
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(ref.isNull());
    gate.open();
    for (int round = 0; round < 500 && done.load() < 2; ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(done.load() == 2);
    // What the workers relayed reaches no dialog.
    QCoreApplication::processEvents();
    CHECK(ref.isNull());
}

TEST_CASE("reads on worker threads reach an open calendar", "[widget]") {
    // The relay's results cross threads as queued signals: the index, then the months.
    fv::ThreadPool pool("calendar-test", 2);
    fv::FakeClock clock(fvtest::todayNoon());
    auto source = std::make_shared<FakeReads>(fvtest::september());
    HistoryCalendarDialog window(source, pool, clock, hc::SUNDAY);
    window.resize(1180, 800);
    window.show();
    for (int round = 0; round < 500 && window.cards().size() < 3; ++round) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(window.isLoaded());
    CHECK(cardIds(window) == Ids{"d1", "d2", "m1"});
    CHECK(window.kindChip(hc::DICTATION_KIND)->text() == QStringLiteral("Dictations  3"));
    window.hide();
}

TEST_CASE("refresh merges bursts and waits while hidden",
          "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::test_refresh_merges_bursts_and_waits_while_hidden
    Calendar calendar;
    HistoryCalendarDialog& window = calendar.window;
    window.hide();
    window.refreshIfVisible();
    CHECK(window.isStale());
    const std::size_t reads = calendar.source->monthReads().size();
    window.show();
    QCoreApplication::processEvents();
    CHECK_FALSE(window.isStale());
    CHECK(calendar.source->monthReads().size() > reads);
    window.refreshIfVisible();
    window.refreshIfVisible();
    CHECK(window.refreshTimer()->isActive());
}

// ---- the painted grid ----

TEST_CASE("grid maps positions and keys to days", "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::test_grid_maps_positions_and_keys_to_days
    Calendar calendar;
    HistoryCalendarDialog& window = calendar.window;
    MonthGrid* grid = window.grid();
    const hc::Day first = grid->weeks()[0][0];
    const QRectF rect = grid->cellRect(0, 0);
    CHECK(grid->dayAt(rect.center()) == first);
    CHECK_FALSE(
        grid->dayAt(QPointF(rect.right() + MonthGrid::kGap / 2.0, rect.center().y())).has_value());

    std::vector<hc::Day> picked;
    std::vector<int> steps;
    int todays = 0;
    QObject::connect(grid, &MonthGrid::daySelected,
                     [&picked](const QDate& day) { picked.push_back(fv::toDay(day)); });
    QObject::connect(grid, &MonthGrid::monthStep, [&steps](int step) { steps.push_back(step); });
    QObject::connect(grid, &MonthGrid::todayRequested, [&todays] { ++todays; });
    window.selectDay(dayOf(2026, 9, 15));
    // Each key moves on from the day the one before it selected.
    for (const Qt::Key key : {Qt::Key_Right, Qt::Key_Down, Qt::Key_Left, Qt::Key_Up}) {
        QTest::keyClick(grid, key);
    }
    CHECK(picked == std::vector<hc::Day>{dayOf(2026, 9, 16), dayOf(2026, 9, 23), dayOf(2026, 9, 22),
                                         dayOf(2026, 9, 15)});
    CHECK(grid->selected() == dayOf(2026, 9, 15));
    QTest::keyClick(grid, Qt::Key_PageDown);
    QTest::keyClick(grid, Qt::Key_T);
    CHECK(steps == std::vector<int>{1});
    CHECK(todays == 1);
}

TEST_CASE("a crowded day shows every kind first", "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::test_a_crowded_day_shows_every_kind_first
    std::vector<hc::CalendarItem> items;
    for (int n = 0; n < 6; ++n) {
        items.push_back(item("d" + std::to_string(n), at(2026, 9, 1, 8, n)));
    }
    items.push_back(item("m", at(2026, 9, 1, 12), {.kind = std::string(hc::MEETING_KIND)}));
    items.push_back(item("f", at(2026, 9, 1, 13), {.kind = std::string(hc::FILE_KIND)}));
    const auto ids = [](const std::vector<const hc::CalendarItem*>& picked) {
        Ids out;
        for (const hc::CalendarItem* each : picked) {
            out.push_back(each->record_id);
        }
        return out;
    };
    CHECK(ids(fv::calendar_view::pick_lines(items, 3)) == Ids{"d0", "m", "f"});
    CHECK(ids(fv::calendar_view::pick_lines(items, 4)) == Ids{"d0", "d1", "m", "f"});
    const std::vector<hc::CalendarItem> two(items.begin(), items.begin() + 2);
    CHECK(ids(fv::calendar_view::pick_lines(two, 3)) == Ids{"d0", "d1"});
}

TEST_CASE("agenda card shows where and how long", "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::test_agenda_card_shows_where_and_how_long
    AgendaCard card(item("x", at(2026, 9, 1, 15, 4),
                         {.kind = std::string(hc::FILE_KIND),
                          .place = hc::Place{std::string(hc::ON), "jed"},
                          .title = "call.m4a",
                          .preview = "Talked about the roof",
                          .seconds = 125}),
                    100);
    std::map<QString, QString> labels;
    for (const QLabel* label : card.findChildren<QLabel*>()) {
        labels[label->objectName()] = label->text();
    }
    CHECK(labels[QStringLiteral("calendarCardTime")] == QStringLiteral("3:04 PM"));
    CHECK(labels[QStringLiteral("calendarKindPill")] == QStringLiteral("File"));
    CHECK(labels[QStringLiteral("calendarPlaceChip")] == QStringLiteral("On jed"));
    CHECK(labels[QStringLiteral("calendarLengthChip")] == QStringLiteral("2m 5s"));
    CHECK(labels[QStringLiteral("calendarCardTitle")] == QStringLiteral("call.m4a"));
    CHECK(labels[QStringLiteral("calendarCardPreview")] == QStringLiteral("Talked about the roof"));
    CHECK(labels[QStringLiteral("calendarMilestone")] == QStringLiteral(u"✦ Your 100th recording"));
}

// ---- the month picker ----

TEST_CASE("the title opens the picker on the year shown",
          "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::test_the_title_opens_the_picker_on_the_year_shown
    Calendar calendar;
    HistoryCalendarDialog& window = calendar.window;
    MonthPicker* picker = window.monthPicker();
    QTest::mouseClick(window.monthTitle(), Qt::LeftButton);
    CHECK(picker->isVisible());
    CHECK(picker->mode() == MonthPicker::Mode::Months);
    CHECK(picker->year() == 2026);
    CHECK(window.monthTitle()->isOpen());
    // A month's tile goes there and closes the picker.
    QTest::mouseClick(picker, Qt::LeftButton, {}, picker->tileCenter(4));
    CHECK_FALSE(picker->isVisible());
    CHECK(monthOf(*window.grid()) == hc::MonthKey{2026, 5});
    CHECK(window.grid()->selected() == dayOf(2026, 5, 4));
    CHECK_FALSE(window.monthTitle()->isOpen());
}

TEST_CASE("the picker leaps between years", "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::test_the_picker_leaps_between_years
    Calendar calendar(fvtest::yearsApart());
    HistoryCalendarDialog& window = calendar.window;
    MonthPicker* picker = window.monthPicker();
    QTest::keyClick(window.grid(), Qt::Key_M);
    CHECK(picker->isVisible());
    CHECK(picker->year() == 2026);
    QTest::keyClick(picker, Qt::Key_PageUp);
    QTest::keyClick(picker, Qt::Key_PageUp);
    CHECK(picker->year() == 2024);
    // No records before 2024, so it stops there.
    CHECK_FALSE(picker->step(-1));

    // The years, newest first; one opens on its busiest recent month.
    QTest::keyClick(picker, Qt::Key_Y);
    CHECK(picker->mode() == MonthPicker::Mode::Years);
    CHECK(picker->pageYears() == std::vector<int>{2026, 2025, 2024});
    QTest::mouseClick(picker, Qt::LeftButton, {}, picker->tileCenter(2));
    CHECK(picker->mode() == MonthPicker::Mode::Months);
    CHECK(picker->year() == 2024);
    QTest::keyClick(picker, Qt::Key_Return);
    CHECK(monthOf(*window.grid()) == hc::MonthKey{2024, 11});
    CHECK(window.grid()->selected() == dayOf(2024, 11, 3));
}

TEST_CASE("picker keys run on through the year and escape steps out",
          "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::
    //     test_picker_keys_run_on_through_the_year_and_escape_steps_out
    Calendar calendar(fvtest::yearsApart());
    HistoryCalendarDialog& window = calendar.window;
    MonthPicker* picker = window.monthPicker();
    window.openMonthPicker();
    picker->setFocusIndex(0);
    QTest::keyClick(picker, Qt::Key_Left);
    CHECK(picker->year() == 2025);
    CHECK(picker->focusIndex() == 11);
    QTest::keyClick(picker, Qt::Key_Y);
    QTest::keyClick(picker, Qt::Key_Escape);
    CHECK(picker->isVisible());
    CHECK(picker->mode() == MonthPicker::Mode::Months);
    QTest::keyClick(picker, Qt::Key_Escape);
    CHECK_FALSE(picker->isVisible());
    CHECK(monthOf(*window.grid()) == hc::MonthKey{2026, 9});

    window.showMonth({2025, 3});
    window.openMonthPicker();
    QTest::keyClick(picker, Qt::Key_T);
    CHECK(monthOf(*window.grid()) == hc::MonthKey{2026, 9});
}

TEST_CASE("a calendar closed with its picker open goes quietly", "[widget]") {
    auto calendar = std::make_unique<Calendar>();
    calendar->window.openMonthPicker();
    REQUIRE(calendar->window.monthPicker()->isVisible());
    QPointer<MonthPicker> picker(calendar->window.monthPicker());
    calendar.reset();
    CHECK(picker.isNull());
}

TEST_CASE("one year of records has no years view", "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::test_one_year_of_records_has_no_years_view
    Calendar calendar;
    MonthPicker* picker = calendar.window.monthPicker();
    calendar.window.openMonthPicker();
    picker->toggleYears();
    CHECK(picker->mode() == MonthPicker::Mode::Months);
    CHECK_FALSE(picker->step(-1));
    CHECK_FALSE(picker->step(1));
}

TEST_CASE("the picker counts follow the filters", "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::test_the_picker_counts_follow_the_filters
    Calendar calendar;
    calendar.window.kindChip(hc::MEETING_KIND)->setChecked(false);
    CHECK(calendar.window.monthPicker()->counts().at(hc::MonthKey{2026, 9}) == 4);
}

TEST_CASE("reopening returns to today", "[test_history_calendar_dialog][widget]") {
    // from tests/test_history_calendar_dialog.py::test_reopening_returns_to_today
    Calendar calendar;
    HistoryCalendarDialog& window = calendar.window;
    window.showMonth({2026, 5});
    window.hide();
    window.show();
    QCoreApplication::processEvents();
    CHECK(monthOf(*window.grid()) == hc::MonthKey{2026, 9});
    CHECK(window.grid()->selected() == kToday);
    CHECK((window.pulseTimer()->isActive() || window.grid()->pulse().has_value()));
}

TEST_CASE("the year strip runs up to now, centres an older month, and a click goes there",
          "[widget]") {
    // history_calendar_dialog._strip_months and YearStrip.mousePressEvent (a09 A09-36).
    Calendar calendar;
    HistoryCalendarDialog& window = calendar.window;
    const auto& months = window.yearStrip()->months();
    REQUIRE(months.size() == 12);
    CHECK(months.front() == hc::MonthKey{2025, 10});
    CHECK(months.back() == hc::MonthKey{2026, 9});

    // Within the last twelve months the window stays on the year up to now.
    window.showMonth({2026, 1});
    CHECK(window.yearStrip()->months().back() == hc::MonthKey{2026, 9});

    // An older month: six after it, eleven before.
    window.showMonth({2024, 3});
    CHECK(window.yearStrip()->months().front() == hc::MonthKey{2023, 10});
    CHECK(window.yearStrip()->months().back() == hc::MonthKey{2024, 9});

    YearStrip* strip = window.yearStrip();
    const double slot = static_cast<double>(strip->width()) / 12.0;
    QTest::mouseClick(strip, Qt::LeftButton, {},
                      QPoint(static_cast<int>(slot * 11.5), strip->height() / 2));
    CHECK(window.grid()->month() == hc::MonthKey{2024, 9});
}
