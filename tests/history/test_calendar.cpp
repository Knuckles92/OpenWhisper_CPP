// tests/test_history_calendar.py: local-day placement, month reads, and the index's stats.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "base/testing/fake_clock.h"
#include "history/calendar.h"
#include "history/calendar_source.h"
#include "history/repository.h"
#include "history_test_support.h"
#include "pyport/errors.h"

namespace history = fv::history;
using history::CalendarIndex;
using history::Day;
using history::Mark;
using history::MonthKey;
using history::Place;

namespace {

// Pacific daylight time: UTC-7, so 05:30 UTC on the 1st is 22:30 the day before.
const std::chrono::time_zone& pdt() {
    return *std::chrono::locate_zone("Etc/GMT+7");
}

Day day(int year, unsigned month, unsigned dom) {
    return Day{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{dom}};
}

Day plus(Day from, int count) {
    return Day{std::chrono::sys_days{from} + std::chrono::days{count}};
}

fv::py::DateTime at(int year, int month, int dom, int hour = 0, int minute = 0) {
    fv::py::DateTime moment;
    moment.year = year;
    moment.month = month;
    moment.day = dom;
    moment.hour = hour;
    moment.minute = minute;
    return moment;
}

std::string iso(Day value) {
    return std::format("{:04}-{:02}-{:02}", static_cast<int>(value.year()),
                       static_cast<unsigned>(value.month()), static_cast<unsigned>(value.day()));
}

const std::string DICTATION{history::DICTATION_KIND};
const std::string FILE_KIND{history::FILE_KIND};
const std::string MEETING{history::MEETING_KIND};
const Place HERE = history::here();

Place from(std::string name) {
    return {std::string(history::FROM), std::move(name)};
}

Place on(std::string name) {
    return {std::string(history::ON), std::move(name)};
}

// The test file's mark(): hour o'clock on `when`, an id from kind, day and hour.
Mark mark(Day when, const std::string& kind = DICTATION, const Place& place = HERE,
          std::optional<std::string> record_id = std::nullopt, double seconds = 0.0, int hour = 9) {
    Mark made;
    made.when = at(static_cast<int>(when.year()), static_cast<int>(unsigned(when.month())),
                   static_cast<int>(unsigned(when.day())), hour);
    made.kind = kind;
    made.place = place;
    made.record_id = record_id.value_or(kind + "-" + iso(when) + "-" + std::to_string(hour));
    made.seconds = seconds;
    return made;
}

struct EntryArgs {
    std::string text = "hello there";
    std::optional<std::string> source_name;
    std::optional<std::string> origin;
    std::optional<double> audio_duration;
    std::optional<std::string> entry_kind;
    std::optional<std::string> title;
};

// The test file's add_entry(): db.put_history_entry of a row.
void add_entry(fv::history_test::TestDatabase& db, std::string id, std::string timestamp,
               const EntryArgs& args = {}) {
    fv::HistoryEntry entry;
    entry.id = std::move(id);
    entry.text = args.text;
    entry.timestamp = std::move(timestamp);
    entry.model = "parakeet";
    entry.source_name = args.source_name;
    if (args.origin) {
        entry.origin_device_id = "dev-1";
    }
    entry.origin_device_name = args.origin;
    entry.audio_duration = args.audio_duration;
    entry.entry_kind = args.entry_kind;
    entry.title = args.title;
    db.repository().put_history_entry(entry);
}

std::vector<std::string> record_ids(const std::vector<history::CalendarItem>& items) {
    std::vector<std::string> ids;
    for (const auto& item : items) {
        ids.push_back(item.record_id);
    }
    return ids;
}

std::set<Day> keys(const std::map<Day, history::DaySummary>& days) {
    std::set<Day> found;
    for (const auto& [when, _summary] : days) {
        found.insert(when);
    }
    return found;
}

} // namespace

// ---- pure helpers ----

// from
// tests/test_history_calendar.py::test_local_wall_time_converts_aware_values_and_keeps_naive_ones
TEST_CASE("local wall time converts aware values and keeps naive ones", "[test_history_calendar]") {
    CHECK(history::local_wall_time("2026-09-01T05:30:00+00:00", pdt()) == at(2026, 8, 31, 22, 30));
    CHECK(history::local_wall_time("2026-09-01T05:30:00Z", pdt()) == at(2026, 8, 31, 22, 30));
    // Legacy entries were stored as local wall time and aren't shifted.
    CHECK(history::local_wall_time("2026-09-01T05:30:00", pdt()) == at(2026, 9, 1, 5, 30));
    CHECK_FALSE(history::local_wall_time("", pdt()));
    CHECK_FALSE(history::local_wall_time("yesterday", pdt()));
}

// from tests/test_history_calendar.py::test_classify_source
TEST_CASE("classify source", "[test_history_calendar]") {
    using Expected = std::pair<std::string, std::string>;
    const auto [source_name, expected] = GENERATE(table<std::optional<std::string>, Expected>({
        {std::nullopt, {"dictation", ""}},
        {"", {"dictation", ""}},
        {"Quick Record", {"dictation", ""}},
        {"Quick Record \xC2\xB7 Email", {"dictation", "Email"}},
        {"interview.wav", {"file", "interview.wav"}},
        {"3 files: a.mp3, b.mp3, c.mp3", {"file", "3 files: a.mp3, b.mp3, c.mp3"}},
        {"Quick Recordings.m4a", {"file", "Quick Recordings.m4a"}},
    }));
    CAPTURE(source_name);
    CHECK(history::classify_source(source_name) == expected);
}

// from tests/test_history_calendar.py::test_classify_source_trusts_the_entry_kind
TEST_CASE("classify source trusts the entry kind", "[test_history_calendar]") {
    using Expected = std::pair<std::string, std::string>;
    const auto [source_name, entry_kind, expected] =
        GENERATE(table<std::optional<std::string>, std::string, Expected>({
            {"Quick Record \xC2\xB7 Email", "dictation", {"dictation", "Email"}},
            {"Quick Record", "command", {"dictation", "Command"}},
            {"Transform \xC2\xB7 Polish", "transform", {"dictation", "Transform \xC2\xB7 Polish"}},
            // Synced rows may carry the bare transform name.
            {"Polish", "transform", {"dictation", "Transform \xC2\xB7 Polish"}},
            {std::nullopt, "transform", {"dictation", "Transform"}},
            {"Quick Record", "file", {"file", "Quick Record"}},
            {"call.m4a", "file", {"file", "call.m4a"}},
        }));
    CAPTURE(source_name, entry_kind);
    CHECK(history::classify_source(source_name, entry_kind) == expected);
}

// from tests/test_history_calendar.py::test_query_bounds_are_a_day_wider_than_the_month
TEST_CASE("query bounds are a day wider than the month", "[test_history_calendar]") {
    using Bounds = std::pair<std::string, std::string>;
    CHECK(history::query_bounds(2026, 9) == Bounds{"2026-08-31", "2026-10-02"});
    CHECK(history::query_bounds(2026, 12) == Bounds{"2026-11-30", "2027-01-02"});
    CHECK(history::query_bounds(2028, 2) == Bounds{"2028-01-31", "2028-03-02"});
}

// from tests/test_history_calendar.py::test_month_weeks_start_on_the_chosen_weekday
TEST_CASE("month weeks start on the chosen weekday", "[test_history_calendar]") {
    const auto sunday_first = history::month_weeks(2026, 9, 6);
    CHECK(sunday_first.front().front() == day(2026, 8, 30));
    CHECK(sunday_first.back().back() == day(2026, 10, 3));
    const auto monday_first = history::month_weeks(2026, 9, 0);
    CHECK(monday_first.front().front() == day(2026, 8, 31));
}

// from tests/test_history_calendar.py::test_shift_month_wraps_years
TEST_CASE("shift month wraps years", "[test_history_calendar]") {
    CHECK(history::shift_month({2026, 12}, 1) == MonthKey{2027, 1});
    CHECK(history::shift_month({2026, 1}, -1) == MonthKey{2025, 12});
    CHECK(history::shift_month({2026, 9}, -21) == MonthKey{2024, 12});
}

// from tests/test_history_calendar.py::test_milestones_and_labels
TEST_CASE("milestones and labels", "[test_history_calendar]") {
    std::vector<std::int64_t> milestones;
    for (std::int64_t n = 1; n < 3001; ++n) {
        if (history::is_milestone(n)) {
            milestones.push_back(n);
        }
    }
    CHECK(milestones == std::vector<std::int64_t>{1, 10, 50, 100, 250, 500, 1000, 2000, 3000});
    CHECK(history::milestone_text(1) == "Your first recording");
    CHECK(history::milestone_text(500) == "Your 500th recording");
    CHECK(history::milestone_text(1000) == "Your 1,000th recording");
    CHECK(history::place_label(std::nullopt) == "Everywhere");
    CHECK(history::place_label(HERE) == "This computer");
    CHECK(history::place_label(from("laptop")) == "From laptop");
    CHECK(history::place_label(on("jed")) == "Kept on jed");
}

// ---- the index ----

// from tests/test_history_calendar.py::test_index_counts_days_and_months_with_filters
TEST_CASE("index counts days and months with filters", "[test_history_calendar]") {
    const CalendarIndex index({
        mark(day(2026, 9, 1), DICTATION, HERE, {}, 10),
        mark(day(2026, 9, 1), FILE_KIND, HERE, {}, 30, 10),
        mark(day(2026, 9, 1), MEETING, HERE, {}, 600, 11),
        mark(day(2026, 9, 3), DICTATION, from("laptop")),
        mark(day(2026, 8, 20)),
    });
    const auto days = index.days(2026, 9);
    CHECK(keys(days) == std::set<Day>{day(2026, 9, 1), day(2026, 9, 3)});
    CHECK(days.at(day(2026, 9, 1)).count == 3);
    CHECK(days.at(day(2026, 9, 1)).seconds == 640);
    CHECK(days.at(day(2026, 9, 1)).by_kind == std::map<std::string, std::int64_t, std::less<>>{
                                                  {DICTATION, 1}, {FILE_KIND, 1}, {MEETING, 1}});
    CHECK(index.month_counts() == std::map<MonthKey, std::int64_t>{{{2026, 9}, 4}, {{2026, 8}, 1}});
    CHECK(index.month_counts(history::KindSet{MEETING}) ==
          std::map<MonthKey, std::int64_t>{{{2026, 9}, 1}});
    CHECK(keys(index.days(2026, 9, std::nullopt, from("laptop"))) ==
          std::set<Day>{day(2026, 9, 3)});
    CHECK(index.places() == std::vector<Place>{HERE, from("laptop")});
}

// from tests/test_history_calendar.py::test_month_stats_pick_the_earliest_of_the_busiest_days
TEST_CASE("month stats pick the earliest of the busiest days", "[test_history_calendar]") {
    std::vector<Mark> marks;
    for (const int hour : {8, 9}) {
        marks.push_back(mark(day(2026, 9, 5), DICTATION, HERE, {}, 0.0, hour));
    }
    for (const int hour : {8, 9}) {
        marks.push_back(mark(day(2026, 9, 2), DICTATION, HERE, {}, 0.0, hour));
    }
    marks.push_back(mark(day(2026, 9, 9)));
    const CalendarIndex index(marks);
    const auto stats = index.month_stats(2026, 9);
    CHECK(stats.count == 5);
    CHECK(stats.active_days == 3);
    CHECK(stats.busiest == std::make_pair(day(2026, 9, 2), std::int64_t{2}));
    CHECK_FALSE(index.month_stats(2026, 10).busiest);
}

// from tests/test_history_calendar.py::test_streak_counts_back_from_today_or_yesterday
TEST_CASE("streak counts back from today or yesterday", "[test_history_calendar]") {
    std::vector<Mark> marks;
    for (const int back : {1, 2, 3, 5}) {
        marks.push_back(mark(plus(day(2026, 9, 27), -back)));
    }
    const CalendarIndex index(marks);
    // Nothing yet today: the run up to yesterday is still alive.
    CHECK(index.streak(day(2026, 9, 27)) == 3);
    CHECK(index.streak(day(2026, 9, 26)) == 3);
    CHECK(index.streak(day(2026, 9, 29)) == 0);
    CHECK(CalendarIndex({mark(day(2026, 9, 27))}).streak(day(2026, 9, 27)) == 1);
}

// from tests/test_history_calendar.py::test_milestones_follow_every_recording_in_time_order
TEST_CASE("milestones follow every recording in time order", "[test_history_calendar]") {
    std::vector<Mark> marks;
    for (int n = 11; n >= 0; --n) {
        Mark made;
        made.when = at(2026, 1, 1, 9 + n);
        made.kind = DICTATION;
        made.record_id = "r" + std::to_string(n);
        marks.push_back(made);
    }
    const CalendarIndex index(marks);
    CHECK(index.milestone("r0") == 1);
    CHECK(index.milestone("r9") == 10);
    CHECK_FALSE(index.milestone("r1"));
    CHECK(index.milestone_days(2026, 1) == std::map<Day, std::int64_t>{{day(2026, 1, 1), 10}});
}

// from tests/test_history_calendar.py::test_nearest_month_prefers_the_closer_then_the_earlier
TEST_CASE("nearest month prefers the closer then the earlier", "[test_history_calendar]") {
    const CalendarIndex index({mark(day(2026, 5, 2)), mark(day(2026, 9, 2))});
    CHECK(index.nearest_month({2026, 7}) == MonthKey{2026, 5});
    CHECK(index.nearest_month({2026, 8}) == MonthKey{2026, 9});
    CHECK(index.nearest_month({2027, 3}) == MonthKey{2026, 9});
    CHECK_FALSE(index.nearest_month({2026, 7}, history::KindSet{MEETING}));
}

// ---- reading from the database ----

// from tests/test_history_calendar.py::test_source_places_utc_entries_on_their_local_day
TEST_CASE("source places utc entries on their local day", "[test_history_calendar]") {
    fv::history_test::TestDatabase db;
    add_entry(db, "utc-late", "2026-09-01T05:30:00+00:00", {.source_name = "Quick Record"});
    add_entry(db, "naive", "2026-09-01T05:30:00",
              {.source_name = "notes.wav", .audio_duration = 12.5});
    add_entry(db, "paired", "2026-09-15T12:00:00+00:00", {.origin = "laptop"});
    add_entry(db, "october", "2026-10-01T06:59:00+00:00");
    const fv::FakeClock clock(fv::FakeClock::default_utc(), "Etc/GMT+7");
    fv::history::NullRemoteRecordSource records;
    history::CalendarSource source(db.database(), clock, nullptr, &records);

    const CalendarIndex index(source.load_index());
    CHECK_FALSE(index.days(2026, 8).empty());
    CHECK(index.days(2026, 8).contains(day(2026, 8, 31)));
    CHECK(keys(index.days(2026, 9)) ==
          std::set<Day>{day(2026, 9, 1), day(2026, 9, 15), day(2026, 9, 30)});
    const auto places = index.places();
    CHECK(std::ranges::find(places, from("laptop")) != places.end());

    const auto items = source.load_month(2026, 9);
    CHECK(record_ids(items) == std::vector<std::string>{"naive", "paired", "october"});
    REQUIRE(items.size() == 3);
    const auto& naive = items[0];
    CHECK(std::tie(naive.kind, naive.title, naive.seconds) ==
          std::make_tuple(FILE_KIND, std::string("notes.wav"), 12.5));
    CHECK(items[1].place == from("laptop"));
    CHECK(items[1].location() ==
          std::make_pair(std::string("From laptop"), std::string("Kept here for laptop, a paired "
                                                                 "computer")));
    const auto august = source.load_month(2026, 8);
    CHECK(record_ids(august) == std::vector<std::string>{"utc-late"});
    REQUIRE(august.size() == 1);
    CHECK(august[0].kind == DICTATION);
    CHECK(august[0].label() == "hello there");
}

// from
// tests/test_history_calendar.py::test_source_sorts_rewrites_with_dictations_and_shows_given_titles
TEST_CASE("source sorts rewrites with dictations and shows given titles",
          "[test_history_calendar]") {
    fv::history_test::TestDatabase db;
    add_entry(db, "cmd", "2026-09-02T15:00:00+00:00",
              {.source_name = "Quick Record", .entry_kind = "command"});
    add_entry(db, "polish", "2026-09-02T16:00:00+00:00",
              {.source_name = "Transform \xC2\xB7 Polish", .entry_kind = "transform"});
    add_entry(db, "upload", "2026-09-02T17:00:00+00:00",
              {.source_name = "call.m4a", .entry_kind = "file", .title = "Call with Sam"});
    const fv::FakeClock clock(fv::FakeClock::default_utc(), "Etc/GMT+7");
    history::CalendarSource source(db.database(), clock);

    std::map<std::string, std::string> kinds;
    for (const Mark& found : source.load_index()) {
        kinds[found.record_id] = found.kind;
    }
    CHECK(kinds == std::map<std::string, std::string>{
                       {"cmd", DICTATION}, {"polish", DICTATION}, {"upload", FILE_KIND}});
    std::map<std::string, history::CalendarItem> items;
    for (auto& item : source.load_month(2026, 9)) {
        items.emplace(item.record_id, item);
    }
    CHECK(items.at("cmd").title == "Command");
    CHECK(items.at("polish").title == "Transform \xC2\xB7 Polish");
    CHECK(std::tie(items.at("upload").kind, items.at("upload").title) ==
          std::make_tuple(FILE_KIND, std::string("Call with Sam")));
}

// Not in OpenWhisper's tests: the host-kept records of a fake host, through the interfaces
// M7 and M9 implement (test_source_merges_host_kept_records_and_copies covers them there).
namespace {

class FakeRecords final : public history::RemoteRecordSource {
public:
    std::map<std::string, std::vector<fv::Json>> remote;
    std::map<std::string, std::set<std::string>> copies;
    std::optional<std::string> fail;

    bool listing_wanted(std::string_view kind) override {
        return remote.contains(std::string(kind)) || fail.has_value();
    }
    std::vector<fv::Json> list_remote(std::string_view kind, std::string_view, int) override {
        if (fail) {
            throw std::runtime_error(*fail);
        }
        std::vector<fv::Json> items = remote[std::string(kind)];
        for (auto& item : items) {
            item["stored_on"] = "jed";
        }
        return items;
    }
    std::vector<std::string> copies_on_host(std::string_view kind,
                                            const std::vector<std::string>& ids) override {
        std::vector<std::string> found;
        for (const auto& id : ids) {
            if (copies[std::string(kind)].contains(id)) {
                found.push_back(id);
            }
        }
        return found;
    }
    std::string host_name() override { return "jed"; }
    fv::HistoryEntry remote_history_entry(const fv::Json& item) override {
        fv::HistoryEntry entry;
        entry.id = item.at("id").get<std::string>();
        entry.text = item.value("text", "");
        entry.timestamp = item.value("timestamp", "");
        entry.stored_on = item.value("stored_on", "");
        return entry;
    }
};

} // namespace

TEST_CASE("source merges a fake host's records and marks copies", "[history_calendar]") {
    fv::history_test::TestDatabase db;
    add_entry(db, "local", "2026-09-03T15:00:00+00:00");
    add_entry(db, "both", "2026-09-04T15:00:00+00:00");
    FakeRecords records;
    records.remote["dictation"] = {
        {{"id", "hosted"},
         {"text", "kept on the host"},
         {"timestamp", "2026-09-05T15:00:00+00:00"},
         {"audio_duration", 3.0}},
        {{"id", "both"}, {"text", "dup"}, {"timestamp", "2026-09-04T15:00:00+00:00"}},
    };
    records.copies["dictation"] = {"both"};
    const fv::FakeClock clock(fv::FakeClock::default_utc(), "Etc/GMT+7");
    history::CalendarSource source(db.database(), clock, nullptr, &records);

    std::vector<std::string> local;
    for (const Mark& found : source.load_index()) {
        local.push_back(found.record_id);
    }
    CHECK(source.remote_wanted());
    const auto remote = source.load_remote(local);
    REQUIRE(remote.size() == 1);
    CHECK(remote[0].record_id == "hosted");
    CHECK(remote[0].place == on("jed"));
    CHECK(remote[0].seconds == 3.0);
    CHECK(source.notice().empty());

    std::map<std::string, history::CalendarItem> items;
    for (auto& item : source.load_month(2026, 9)) {
        items.emplace(item.record_id, item);
    }
    CHECK(items.size() == 3);
    CHECK(items.at("both").location() ==
          std::make_pair(std::string("Also on jed"), std::string("Kept here and on jed")));
    CHECK(items.at("hosted").location().first == "On jed");
    CHECK(std::get<fv::HistoryEntry>(items.at("hosted").source).stored_on == "jed");
    CHECK(items.at("local").location() == std::make_pair(std::string(), std::string()));
}

TEST_CASE("source reports a fake host that cannot be reached", "[history_calendar]") {
    fv::history_test::TestDatabase db;
    FakeRecords records;
    records.fail = "jed isn't reachable right now.";
    const fv::FakeClock clock;
    history::CalendarSource source(db.database(), clock, nullptr, &records);
    CHECK(source.load_remote({}).empty());
    CHECK(source.notice() == "jed isn't reachable right now.");
    records.fail = "";
    CHECK(source.load_remote({}).empty());
    CHECK(source.notice() == "The host couldn't be asked.");
}

TEST_CASE("a source moved out of a freed one keeps its null meetings and host",
          "[history_calendar]") {
    fv::history_test::TestDatabase db;
    add_entry(db, "local", "2026-09-03T15:00:00+00:00");
    const fv::FakeClock clock(fv::FakeClock::default_utc(), "Etc/GMT+7");
    auto first = std::make_unique<history::CalendarSource>(db.database(), clock);
    history::CalendarSource moved = std::move(*first);
    first.reset();
    CHECK_FALSE(moved.remote_wanted());
    CHECK(moved.load_remote({"local"}).empty());
    CHECK(moved.load_index().size() == 1);
    CHECK(record_ids(moved.load_month(2026, 9)) == std::vector<std::string>{"local"});
    const history::CalendarSource copied = moved;
    CHECK(copied.notice().empty());
}

TEST_CASE("a converted moment past year 9999 overflows as in Python", "[history_calendar]") {
    CHECK_THROWS_AS(history::local_wall_time("9999-12-31T23:00:00-05:00", pdt()),
                    fv::py::OverflowError);
    CHECK_THROWS_AS(history::local_wall_time("0001-01-01T00:00:00+05:00", pdt()),
                    fv::py::OverflowError);
    CHECK(history::local_wall_time("9999-12-31T20:00:00-03:00", pdt()) == at(9999, 12, 31, 16));
}
