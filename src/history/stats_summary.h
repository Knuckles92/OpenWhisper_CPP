#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "history/dictation_stats.h"

// services/dictation_stats.py, the summary half: everything the Stats window shows, from the
// dictation_stats rows. Words are the words said; pace is those words per minute of recording,
// over recordings of a second or more; time saved is typing them at 40 words a minute less
// saying them at your pace. Files, walkthroughs and meetings are counted apart from the words.
// Days are local to the clock's zone.
namespace fv::dictation_stats {

// The window's rows beyond dictation, in the order it lists them.
inline constexpr std::array<std::string_view, 7> FEATURES = {
    "meetings", "files", "walkthroughs", "actions", "commands", "snippets", "dictionary"};
// Destinations an Actions row names.
inline constexpr std::size_t TOP_TARGETS = 3;
// Shorter recordings say little about pace (seconds).
inline constexpr double MIN_PACE_SECONDS = 1.0;
inline constexpr std::size_t TOP_APPS = 5;
// An average typist's pace in words a minute, the baseline for time saved.
inline constexpr int TYPING_WPM = 40;
// Days the activity map can show: 53 weeks, so a full year always fits.
inline constexpr int ACTIVITY_DAYS = 371;
// All time shows a bar per month: at most MONTH_BARS, at least MIN_MONTH_BARS.
inline constexpr std::size_t MONTH_BARS = 12;
inline constexpr std::size_t MIN_MONTH_BARS = 6;
// The spans the window switches between: key and days, none for all time.
inline constexpr std::array<std::pair<std::string_view, std::optional<int>>, 3> PERIODS = {{
    {"week", 7},
    {"month", 30},
    {"all", std::nullopt},
}};

// A local day.
using Day = std::chrono::year_month_day;

// One feature's counts for a span; which fields are filled depends on key.
struct FeatureStats {
    std::string key;
    // Meetings, files, walkthroughs, actions sent, commands run, snippets used or dictionary
    // corrections.
    std::int64_t count = 0;
    // Audio length in seconds, for meetings, files and walkthroughs.
    double seconds = 0.0;
    // Words transcribed (meetings, files), rewritten (commands) or written for you (snippets).
    std::int64_t words = 0;
    // Screenshots kept, for walkthroughs.
    std::int64_t shots = 0;
    // (action name, times sent), most used first.
    std::vector<std::pair<std::string, std::int64_t>> top;
    // count per bar of the span's chart, oldest first.
    std::vector<std::int64_t> trend;

    friend bool operator==(const FeatureStats&, const FeatureStats&) = default;
};

// The counts for one span: the last 7 days, the last 30, or all time.
struct PeriodStats {
    std::string key = "week";
    // Days in the span, or none for all time.
    std::optional<int> days = 7;
    std::int64_t words = 0;
    // Words in the same number of days just before; none for all time.
    std::optional<std::int64_t> previous_words;
    std::int64_t dictations = 0;
    // Commands, transforms and actions: text the AI rewrote when asked.
    std::int64_t rewrites = 0;
    // Rounded to 0.1, half to even.
    double average_wpm = 0.0;
    std::int64_t words_cleaned_up = 0;
    // (app_name, words), most words first.
    std::vector<std::pair<std::string, std::int64_t>> top_apps;
    // (start, words), oldest first: a bar per day, or per month (its first day) for all time.
    std::vector<std::pair<Day, std::int64_t>> bars;
    // Words by local hour, midnight first.
    std::array<std::int64_t, 24> hours{};
    // Words by weekday, Monday first.
    std::array<std::int64_t, 7> weekdays{};
    // Words by weekday (Monday first), then local hour.
    std::array<std::array<std::int64_t, 24>, 7> grid{};
    // (key, words) breakdowns of the words dictated, most words first. An empty key is an app of
    // no known kind, no AI cleanup, or no language. Empty when no row names one.
    std::vector<std::pair<std::string, std::int64_t>> categories;
    std::vector<std::pair<std::string, std::int64_t>> cleanup_levels;
    std::vector<std::pair<std::string, std::int64_t>> languages;
    // One per FEATURES key, in that order.
    std::vector<FeatureStats> features;

    // The feature's counts, or empty counts for a key it does not hold.
    FeatureStats feature(std::string_view key) const;
    // Minutes typing these words would take, less saying them; 0 without a pace above
    // TYPING_WPM.
    double minutes_saved() const;

    friend bool operator==(const PeriodStats&, const PeriodStats&) = default;
};

struct StatsSummary {
    std::int64_t words_all_time = 0;
    // Every row counted, of any kind.
    std::int64_t entries = 0;
    std::int64_t day_streak = 0;
    std::int64_t longest_streak = 0;
    bool dictated_today = false;
    std::optional<Day> first_day;
    // (day, words) for the last ACTIVITY_DAYS days, oldest first.
    std::vector<std::pair<Day, std::int64_t>> activity;
    // One per PERIODS key, in that order.
    std::vector<PeriodStats> periods;

    // The period's counts, or empty ones with the span's days for a key it does not hold.
    // Throws py::PyError of type "KeyError" for a key PERIODS does not name.
    PeriodStats period(std::string_view key) const;

    friend bool operator==(const StatsSummary&, const StatsSummary&) = default;
};

// Everything the Stats window shows. Counts the earlier history and the finished meetings
// first (ensure_backfilled, count_features), then reads every row. `today` is the local day to
// count back from: the clock's local date when none.
// Affinity: blocking and reads the whole table: never on the GUI thread.
StatsSummary load_summary(StatsRepository& stats, const Clock& clock = system_clock(),
                          std::optional<Day> today = std::nullopt);

} // namespace fv::dictation_stats
