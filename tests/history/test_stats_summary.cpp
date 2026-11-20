// The Stats summary of services/dictation_stats.py (load_summary):
//   tests/test_personalize_s9_stats.py (the 9 summary tests),
//   tests/test_stats_features.py (the 4 summary tests; the window's are M2.27's).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/testing/fake_clock.h"
#include "history/dictation_stats.h"
#include "history/stats_summary.h"
#include "history_test_support.h"
#include "pyport/errors.h"

namespace stats = fv::dictation_stats;
using fv::Json;
using stats::Day;

namespace {

constexpr Day TODAY{std::chrono::year{2026}, std::chrono::October, std::chrono::day{6}};

Day plus(Day from, int count) {
    return Day{std::chrono::sys_days{from} + std::chrono::days{count}};
}

Day ymd(int year, unsigned month, unsigned dom) {
    return Day{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{dom}};
}

// date.weekday(): Monday is 0.
std::size_t weekday(Day day) {
    return std::chrono::weekday{std::chrono::sys_days{day}}.iso_encoding() - 1;
}

// _at: an aware UTC timestamp for `hour` o'clock Pacific time (UTC-7) on `day`.
std::string at(Day day, int hour = 12) {
    const std::chrono::sys_seconds instant =
        std::chrono::sys_days{day} + std::chrono::hours{hour + 7};
    return std::format("{:%Y-%m-%dT%H:%M:%S}+00:00", instant);
}

struct Record {
    std::string text;
    Day day = TODAY;
    std::optional<std::string> raw;
    std::optional<double> seconds;
    std::string kind = "dictation";
    std::optional<std::string> app;
    std::optional<std::string> entry_id;
    int hour = 12;
    std::optional<std::string> app_category;
    std::optional<std::string> cleanup_level;
    std::optional<std::string> language;
    Json counts = Json::object();
};

struct Stats {
    fv::history_test::TestDatabase db;
    stats::StatsRepository repository{db.database()};

    // _record: counted from its fields alone.
    void record(const Record& args) {
        stats::EntryFields fields;
        fields.text = args.text;
        fields.raw_text = args.raw;
        fields.audio_duration = args.seconds;
        fields.entry_kind = args.kind;
        fields.app_name = args.app;
        fields.id = args.entry_id;
        fields.timestamp = at(args.day, args.hour);
        fields.app_category = args.app_category;
        fields.cleanup_level = args.cleanup_level;
        fields.language = args.language;
        fields.stats_counts = args.counts;
        repository.record(fields, nullptr);
    }

    // load_summary(today=today, tz=timezone(timedelta(hours=offset))).
    stats::StatsSummary summary(Day today = TODAY, int offset_hours = -7) {
        const std::string zone = offset_hours < 0 ? std::format("Etc/GMT+{}", -offset_hours)
                                                  : std::format("Etc/GMT-{}", offset_hours);
        const fv::FakeClock clock(fv::FakeClock::default_utc(), zone);
        return stats::load_summary(repository, clock, today);
    }
    stats::PeriodStats week() { return summary().period("week"); }
};

std::map<Day, std::int64_t> as_map(const std::vector<std::pair<Day, std::int64_t>>& bars) {
    return {bars.begin(), bars.end()};
}

using Counts = std::vector<std::pair<std::string, std::int64_t>>;

} // namespace

// from tests/test_personalize_s9_stats.py::test_the_summary_counts_days_pace_streak_and_apps
TEST_CASE("the summary counts days pace streak and apps", "[test_personalize_s9_stats]") {
    Stats s;
    s.repository.reset();
    s.record({.text = "one two three four five six", .seconds = 3.0, .app = "Slack"}); // 120 wpm
    s.record({.text = "one two three", .day = plus(TODAY, -1), .seconds = 0.5, .app = "Outlook"});
    s.record({.text = "one two",
              .day = plus(TODAY, -2),
              .raw = "1 2 3 4",
              .seconds = 60.0,
              .app = "Slack"});
    s.record({.text = "old words here", .day = plus(TODAY, -7), .app = "Notion"});
    s.record({.text = "older", .day = plus(TODAY, -20), .app = "Word"});
    s.record({.text = "rewrite", .raw = "draft", .kind = "transform"});

    const auto summary = s.summary();
    const auto week = summary.period("week");
    const auto month = summary.period("month");
    const auto all_time = summary.period("all");

    CHECK(summary.words_all_time == 6 + 3 + 4 + 3 + 1);
    CHECK(all_time.words == 6 + 3 + 4 + 3 + 1);
    CHECK(week.words == 6 + 3 + 4);
    CHECK(week.previous_words == 3);
    CHECK_FALSE(all_time.previous_words);
    CHECK(month.words == 17);
    CHECK(month.previous_words == 0);
    CHECK(all_time.average_wpm == Catch::Approx((6 + 4) / (63.0 / 60)).margin(0.1));
    CHECK(all_time.words_cleaned_up == 4 + 1);
    CHECK(std::make_pair(all_time.dictations, all_time.rewrites) ==
          std::pair<std::int64_t, std::int64_t>{5, 1});
    CHECK(std::make_pair(week.dictations, week.rewrites) ==
          std::pair<std::int64_t, std::int64_t>{3, 1});
    CHECK(summary.day_streak == 3);
    CHECK(summary.longest_streak == 3);
    CHECK(summary.dictated_today);
    CHECK(summary.first_day == plus(TODAY, -20));
    CHECK(all_time.top_apps == Counts{{"Slack", 10}, {"Notion", 3}, {"Outlook", 3}, {"Word", 1}});
    CHECK(week.top_apps == Counts{{"Slack", 10}, {"Outlook", 3}});
    std::vector<Day> week_days;
    for (const auto& [when, _words] : week.bars) {
        week_days.push_back(when);
    }
    std::vector<Day> expected_days;
    for (int offset = 6; offset >= 0; --offset) {
        expected_days.push_back(plus(TODAY, -offset));
    }
    CHECK(week_days == expected_days);
    CHECK(month.bars.size() == 30);
    CHECK(as_map(month.bars).at(plus(TODAY, -7)) == 3);
    CHECK(as_map(week.bars).at(TODAY) == 6);
    CHECK(summary.entries == 6);
}

// from tests/test_personalize_s9_stats.py::test_time_of_day_and_weekday_follow_local_time
TEST_CASE("time of day and weekday follow local time", "[test_personalize_s9_stats]") {
    Stats s;
    s.repository.reset();
    s.record({.text = "one two three", .hour = 9}); // TODAY is a Tuesday
    s.record({.text = "four five", .hour = 21});
    s.record({.text = "six", .day = plus(TODAY, -1), .hour = 9});
    const auto week = s.week();
    CHECK(week.hours[9] == 4);
    CHECK(week.hours[21] == 2);
    std::int64_t total = 0;
    for (const auto words : week.hours) {
        total += words;
    }
    CHECK(total == 6);
    CHECK(week.weekdays[weekday(TODAY)] == 5);
    CHECK(week.weekdays[weekday(TODAY) - 1] == 1);
}

// from tests/test_personalize_s9_stats.py::test_time_saved_compares_typing_with_your_pace
TEST_CASE("time saved compares typing with your pace", "[test_personalize_s9_stats]") {
    stats::PeriodStats period;
    period.words = 1200;
    period.average_wpm = 120.0;
    CHECK(period.minutes_saved() == Catch::Approx(1200.0 / stats::TYPING_WPM - 1200.0 / 120));
    period.average_wpm = 0.0;
    CHECK(period.minutes_saved() == 0.0);
}

// from
// tests/test_personalize_s9_stats.py::test_all_time_bars_are_months_back_to_the_first_dictation
TEST_CASE("all time bars are months back to the first dictation", "[test_personalize_s9_stats]") {
    Stats s;
    s.repository.reset();
    s.record({.text = "words here", .day = ymd(2025, 6, 15)});
    s.record({.text = "more", .day = ymd(2026, 9, 1)});
    auto bars = s.summary().period("all").bars;
    CHECK(bars.size() == stats::MONTH_BARS);
    CHECK(bars.back() == std::make_pair(ymd(2026, 10, 1), std::int64_t{0}));
    CHECK(as_map(bars).at(ymd(2026, 9, 1)) == 1);
    s.repository.reset();
    s.record({.text = "fresh start"});
    bars = s.summary().period("all").bars;
    std::vector<Day> months;
    for (const auto& [month, _words] : bars) {
        months.push_back(month);
    }
    std::vector<Day> expected;
    for (unsigned m = 5; m <= 10; ++m) {
        expected.push_back(ymd(2026, m, 1));
    }
    CHECK(months == expected);
}

// from
// tests/test_personalize_s9_stats.py::test_the_streak_survives_until_the_day_ends_and_breaks_on_a_gap
TEST_CASE("the streak survives until the day ends and breaks on a gap",
          "[test_personalize_s9_stats]") {
    Stats s;
    s.repository.reset();
    for (const int offset : {1, 2, 3, 5, 6, 7, 8}) {
        s.record({.text = "words", .day = plus(TODAY, -offset)});
    }
    const auto summary = s.summary();
    CHECK(summary.day_streak == 3);
    CHECK_FALSE(summary.dictated_today);
    CHECK(summary.longest_streak == 4);
    CHECK(s.summary(plus(TODAY, 1)).day_streak == 0);
}

// from tests/test_personalize_s9_stats.py::test_activity_covers_a_year_ending_today
TEST_CASE("activity covers a year ending today", "[test_personalize_s9_stats]") {
    Stats s;
    s.repository.reset();
    s.record({.text = "today's words"});
    s.record({.text = "long ago", .day = plus(TODAY, -400)});
    const auto activity = s.summary().activity;
    CHECK(activity.size() == static_cast<std::size_t>(stats::ACTIVITY_DAYS));
    CHECK(activity.back() == std::make_pair(TODAY, std::int64_t{2}));
    std::int64_t total = 0;
    for (const auto& [_day, words] : activity) {
        total += words;
    }
    CHECK(total == 2);
}

// from tests/test_personalize_s9_stats.py::test_days_follow_the_local_zone
TEST_CASE("days follow the local zone", "[test_personalize_s9_stats]") {
    Stats s;
    s.repository.reset();
    // 11 pm Pacific on the 5th is already the 6th in Tokyo.
    s.record({.text = "late night words", .day = plus(TODAY, -1), .hour = 23});
    const auto pacific = as_map(s.summary(TODAY, -7).period("week").bars);
    const auto tokyo = as_map(s.summary(TODAY, 9).period("week").bars);
    CHECK(pacific.at(plus(TODAY, -1)) == 3);
    CHECK(pacific.at(TODAY) == 0);
    CHECK(tokyo.at(TODAY) == 3);
}

// from tests/test_personalize_s9_stats.py::test_top_apps_keep_the_five_busiest
TEST_CASE("top apps keep the five busiest", "[test_personalize_s9_stats]") {
    Stats s;
    s.repository.reset();
    int count = 1;
    for (const char* app : {"A", "B", "C", "D", "E", "F"}) {
        std::string text;
        for (int i = 0; i < count; ++i) {
            text += i == 0 ? "w" : " w";
        }
        s.record({.text = text, .app = app});
        ++count;
    }
    s.record({.text = "no app here at all"});
    std::vector<std::string> names;
    for (const auto& [name, _words] : s.summary().period("all").top_apps) {
        names.push_back(name);
    }
    CHECK(names == std::vector<std::string>{"F", "E", "D", "C", "B"});
}

// from tests/test_personalize_s9_stats.py::test_the_next_milestone_is_the_smallest_above
TEST_CASE("the next milestone is the smallest above", "[test_personalize_s9_stats]") {
    const auto [words, milestone] = GENERATE(table<std::int64_t, std::int64_t>({
        {0, 1'000},
        {999, 1'000},
        {1'000, 5'000},
        {57'224, 100'000},
        {1'000'000, 2'000'000},
        {2'400'000, 3'000'000},
    }));
    CAPTURE(words);
    CHECK(stats::next_milestone(words) == milestone);
}

// from
// tests/test_stats_features.py::test_each_feature_gets_its_own_counts_and_none_of_the_dictated_words
TEST_CASE("each feature gets its own counts and none of the dictated words",
          "[test_stats_features]") {
    Stats s;
    s.repository.reset();
    s.record({.text = "eight words said aloud for the real count",
              .raw = "eight words said aloud for the real count",
              .counts = {{"snippets", 2}, {"snippet_words", 40}, {"corrections", 3}}});
    s.record({.text = "a walkthrough",
              .seconds = 120.0,
              .kind = "walkthrough",
              .counts = {{"shots", 5}}});
    s.record({.text = "to claude", .kind = "action", .counts = {{"target", "Claude"}}});
    s.record({.text = "to claude again",
              .day = plus(TODAY, -1),
              .kind = "action",
              .counts = {{"target", "Claude"}}});
    s.record({.text = "to the hook", .kind = "action", .counts = {{"target", "Webhook"}}});
    s.record({.text = "Formal now.", .raw = "formal nw", .kind = "command"});
    s.record({.text = "a talk of five words", .seconds = 600.0, .kind = "file"});

    const auto summary = s.summary();
    const auto week = summary.period("week");

    CHECK(summary.words_all_time == 8);
    CHECK(week.words == 8);
    std::vector<std::string> keys;
    for (const auto& feature : week.features) {
        keys.push_back(feature.key);
    }
    CHECK(keys == std::vector<std::string>(stats::FEATURES.begin(), stats::FEATURES.end()));
    const auto walkthroughs = week.feature("walkthroughs");
    CHECK(walkthroughs.count == 1);
    CHECK(walkthroughs.shots == 5);
    CHECK(walkthroughs.seconds == 120.0);
    const auto actions = week.feature("actions");
    CHECK(actions.count == 3);
    CHECK(actions.top == Counts{{"Claude", 2}, {"Webhook", 1}});
    CHECK(actions.trend == std::vector<std::int64_t>{0, 0, 0, 0, 0, 1, 2});
    CHECK(week.feature("commands").count == 1);
    CHECK(week.feature("commands").words == 2);
    CHECK(week.feature("snippets").count == 2);
    CHECK(week.feature("snippets").words == 40);
    CHECK(week.feature("dictionary").count == 3);
    CHECK(week.feature("files").count == 1);
    CHECK(week.feature("files").words == 5);
    // A file's cleanup isn't words AI changed in something you dictated.
    CHECK(week.words_cleaned_up == 2);
    CHECK(week.feature("meetings").count == 0);
}

// from tests/test_stats_features.py::test_a_feature_alone_keeps_no_streak_but_fills_the_window
TEST_CASE("a feature alone keeps no streak but fills the window", "[test_stats_features]") {
    Stats s;
    s.repository.reset();
    s.record({.text = "only a file", .kind = "file"});
    const auto summary = s.summary();
    CHECK(summary.entries == 1);
    CHECK(summary.day_streak == 0);
    CHECK_FALSE(summary.dictated_today);
    CHECK_FALSE(summary.first_day);
}

// from tests/test_stats_features.py::test_words_break_down_by_app_kind_cleanup_language_and_hour
TEST_CASE("words break down by app kind cleanup language and hour", "[test_stats_features]") {
    Stats s;
    s.repository.reset();
    s.record({.text = "one two three four",
              .hour = 9,
              .app_category = "work",
              .cleanup_level = "light",
              .language = "en"});
    s.record({.text = "one two", .hour = 15, .app_category = "email", .language = "it"});
    s.record({.text = "one", .day = plus(TODAY, -1), .hour = 9});

    const auto week = s.week();

    CHECK(week.categories == Counts{{"work", 4}, {"email", 2}, {"", 1}});
    CHECK(week.cleanup_levels == Counts{{"light", 4}, {"", 3}});
    CHECK(week.languages == Counts{{"en", 4}, {"it", 2}, {"", 1}});
    CHECK(week.grid[weekday(TODAY)][9] == 4);
    CHECK(week.grid[weekday(TODAY)][15] == 2);
    CHECK(week.grid[weekday(plus(TODAY, -1))][9] == 1);
    CHECK(week.hours[9] == 5);
    CHECK(week.weekdays[weekday(TODAY)] == 6);
}

// from tests/test_stats_features.py::test_a_breakdown_no_row_names_is_left_out
TEST_CASE("a breakdown no row names is left out", "[test_stats_features]") {
    Stats s;
    s.repository.reset();
    s.record({.text = "no context at all"});
    const auto week = s.week();
    CHECK(week.categories.empty());
    CHECK(week.cleanup_levels.empty());
    CHECK(week.languages.empty());
}

TEST_CASE("a period the summary does not name is a KeyError", "[history_stats_summary]") {
    const stats::StatsSummary empty;
    const auto month = empty.period("month");
    CHECK(month.key == "month");
    CHECK(month.days == 30);
    CHECK_FALSE(empty.period("all").days);
    CHECK_THROWS_AS(empty.period("year"), fv::py::PyError);
    CHECK(empty.period("week").feature("files").key == "files");
}

TEST_CASE("today defaults to the clock's local date", "[history_stats_summary]") {
    Stats s;
    s.repository.reset();
    s.record({.text = "late night words", .day = plus(TODAY, -1), .hour = 23});
    // 2026-10-06T06:00Z: 23:00 on the 5th in Pacific daylight time, 15:00 on the 6th in Tokyo.
    const auto instant = std::chrono::sys_days{TODAY} + std::chrono::hours{6};
    const fv::FakeClock pacific(instant, "Etc/GMT+7");
    const fv::FakeClock tokyo(instant, "Etc/GMT-9");
    CHECK(stats::load_summary(s.repository, pacific).activity.back().first == plus(TODAY, -1));
    CHECK(stats::load_summary(s.repository, pacific).dictated_today);
    CHECK(stats::load_summary(s.repository, tokyo).activity.back().first == TODAY);
    CHECK(stats::load_summary(s.repository, tokyo).dictated_today);
}
