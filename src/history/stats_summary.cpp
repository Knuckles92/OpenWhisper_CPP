#include "history/stats_summary.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <tuple>

#include "history/detail/time.h"
#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::dictation_stats {

namespace {

using std::chrono::days;
using std::chrono::sys_days;

using Counts = std::vector<std::pair<std::string, std::int64_t>>;

// dictation_stats._Row: one stats row as the summary reads it.
struct Row {
    py::DateTime moment; // naive local wall time
    Day day;
    std::string kind;
    std::optional<std::string> app_name;
    std::int64_t words = 0;
    std::int64_t edited = 0;
    std::optional<double> seconds;
    std::int64_t final_words = 0;
    std::string category;
    std::string cleanup;
    std::string language;
    std::string target;
    std::int64_t snippets = 0;
    std::int64_t snippet_words = 0;
    std::int64_t corrections = 0;
    std::int64_t shots = 0;
};

bool is_counted(std::string_view kind) {
    return std::ranges::find(COUNTED_KINDS, kind) != COUNTED_KINDS.end();
}

bool is_feature(std::string_view kind) {
    return std::ranges::find(FEATURE_KINDS, kind) != FEATURE_KINDS.end();
}

Day day_of(const py::DateTime& moment) {
    return Day{std::chrono::year{moment.year},
               std::chrono::month{static_cast<unsigned>(moment.month)},
               std::chrono::day{static_cast<unsigned>(moment.day)}};
}

Day plus_days(Day day, std::int64_t count) {
    return Day{sys_days{day} + days{count}};
}

Day first_of_month(Day day) {
    return Day{day.year(), day.month(), std::chrono::day{1}};
}

// date.weekday(): Monday is 0.
int weekday(Day day) {
    return static_cast<int>(std::chrono::weekday{sys_days{day}}.iso_encoding()) - 1;
}

std::vector<Row> read_rows(StatsRepository& stats, const std::chrono::time_zone& zone) {
    std::vector<Row> rows;
    for (const StatRow& stat : stats.rows()) {
        if (!is_counted(stat.entry_kind) && !is_feature(stat.entry_kind)) {
            continue;
        }
        const auto moment = local_time(stat.timestamp, zone);
        if (!moment) {
            continue;
        }
        Row row;
        row.moment = *moment;
        row.day = day_of(*moment);
        row.kind = stat.entry_kind;
        if (stat.app_name && !stat.app_name->empty()) {
            row.app_name = stat.app_name;
        }
        row.words = stat.spoken_words;
        row.edited = stat.words_edited;
        row.seconds = stat.audio_seconds;
        row.final_words = stat.final_words;
        row.category = stat.app_category.value_or("");
        row.cleanup = stat.cleanup_level.value_or("");
        row.language = stat.language.value_or("");
        row.target = stat.target.value_or("");
        row.snippets = stat.snippets;
        row.snippet_words = stat.snippet_words;
        row.corrections = stat.corrections;
        row.shots = stat.shots;
        rows.push_back(std::move(row));
    }
    return rows;
}

// Words per local day; a day with only a rewrite is there with no words.
std::map<Day, std::int64_t> words_by_day(const std::vector<Row>& rows) {
    std::map<Day, std::int64_t> by_day;
    for (const Row& row : rows) {
        if (is_counted(row.kind)) {
            by_day[row.day] += row.words;
        }
    }
    return by_day;
}

std::int64_t get(const std::map<Day, std::int64_t>& by_day, Day day) {
    const auto found = by_day.find(day);
    return found == by_day.end() ? 0 : found->second;
}

std::int64_t streak(const std::map<Day, std::int64_t>& active, Day today) {
    Day day = active.contains(today) ? today : plus_days(today, -1);
    std::int64_t count = 0;
    while (active.contains(day)) {
        ++count;
        day = plus_days(day, -1);
    }
    return count;
}

std::int64_t longest_streak(const std::map<Day, std::int64_t>& active) {
    std::int64_t longest = 0;
    std::int64_t run = 0;
    std::optional<Day> previous;
    for (const auto& [day, _words] : active) {
        run = previous && sys_days{day} - sys_days{*previous} == days{1} ? run + 1 : 1;
        longest = std::max(longest, run);
        previous = day;
    }
    return longest;
}

double pace(const std::vector<const Row*>& rows) {
    std::int64_t words = 0;
    double seconds = 0.0;
    for (const Row* row : rows) {
        if (row->seconds && *row->seconds >= MIN_PACE_SECONDS && row->words != 0) {
            words += row->words;
            seconds += *row->seconds;
        }
    }
    if (seconds == 0.0) {
        return 0.0;
    }
    return py::round_ndigits(static_cast<double>(words) / (seconds / 60.0), 1);
}

// Counts by name in first-seen order, then most first, ties by the case-folded name.
Counts most_first_folded(Counts counts, std::size_t limit) {
    std::ranges::stable_sort(counts, [](const auto& a, const auto& b) {
        if (a.second != b.second) {
            return a.second > b.second;
        }
        return py::casefold(a.first) < py::casefold(b.first);
    });
    if (counts.size() > limit) {
        counts.resize(limit);
    }
    return counts;
}

void add(Counts& counts, const std::string& key, std::int64_t amount) {
    const auto found = std::ranges::find(counts, key, &Counts::value_type::first);
    if (found == counts.end()) {
        counts.emplace_back(key, amount);
    } else {
        found->second += amount;
    }
}

Counts top_apps(const std::vector<const Row*>& rows) {
    Counts by_app;
    for (const Row* row : rows) {
        if (row->app_name && row->words != 0) {
            add(by_app, *row->app_name, row->words);
        }
    }
    return most_first_folded(std::move(by_app), TOP_APPS);
}

// The `count` days ending today, oldest first.
std::vector<Day> days_back(Day today, std::int64_t count) {
    std::vector<Day> result;
    for (std::int64_t offset = count - 1; offset >= 0; --offset) {
        result.push_back(plus_days(today, -offset));
    }
    return result;
}

// Words per month back to the first dictation, within MIN_MONTH_BARS..MONTH_BARS months.
std::vector<std::pair<Day, std::int64_t>> month_bars(const std::map<Day, std::int64_t>& by_day,
                                                     Day today) {
    std::map<Day, std::int64_t> by_month;
    for (const auto& [day, words] : by_day) {
        by_month[first_of_month(day)] += words;
    }
    const Day first = by_month.empty() ? first_of_month(today) : by_month.begin()->first;
    std::vector<Day> months = {first_of_month(today)};
    while (months.size() < MONTH_BARS &&
           (months.back() > first || months.size() < MIN_MONTH_BARS)) {
        months.push_back(first_of_month(plus_days(months.back(), -1)));
    }
    std::vector<std::pair<Day, std::int64_t>> bars;
    for (auto month = months.rbegin(); month != months.rend(); ++month) {
        bars.emplace_back(*month, get(by_month, *month));
    }
    return bars;
}

// Words dictated per key, most first; nothing when no row names one.
Counts shares(const std::vector<const Row*>& rows,
              const std::function<std::string(const Row&)>& key) {
    Counts by_key;
    for (const Row* row : rows) {
        if (row->words != 0) {
            add(by_key, key(*row), row->words);
        }
    }
    if (std::ranges::none_of(by_key, [](const auto& item) { return !item.first.empty(); })) {
        return {};
    }
    std::ranges::stable_sort(by_key, [](const auto& a, const auto& b) {
        return std::tie(b.second, a.first) < std::tie(a.second, b.first);
    });
    return by_key;
}

std::vector<FeatureStats> features(const std::vector<const Row*>& rows,
                                   const std::vector<std::pair<Day, std::int64_t>>& bars,
                                   bool monthly) {
    std::map<Day, std::size_t> slots;
    for (std::size_t index = 0; index < bars.size(); ++index) {
        slots[bars[index].first] = index;
    }
    const auto feature = [&](std::string_view key, const std::vector<const Row*>& chosen,
                             const std::function<std::int64_t(const Row&)>& count) {
        FeatureStats stats;
        stats.key = std::string(key);
        stats.trend.assign(bars.size(), 0);
        for (const Row* row : chosen) {
            const auto slot = slots.find(monthly ? first_of_month(row->day) : row->day);
            if (slot != slots.end()) {
                stats.trend[slot->second] += count(*row);
            }
            stats.count += count(*row);
        }
        return stats;
    };
    const auto once = [](const Row&) -> std::int64_t {
        return 1;
    };
    const auto of = [&](std::initializer_list<std::string_view> kinds) {
        std::vector<const Row*> chosen;
        for (const Row* row : rows) {
            if (std::ranges::find(kinds, row->kind) != kinds.end()) {
                chosen.push_back(row);
            }
        }
        return chosen;
    };
    const auto audio = [](const std::vector<const Row*>& chosen) {
        double seconds = 0.0;
        for (const Row* row : chosen) {
            seconds += row->seconds.value_or(0.0);
        }
        return seconds;
    };
    const auto sum = [](const std::vector<const Row*>& chosen, std::int64_t Row::*member) {
        std::int64_t total = 0;
        for (const Row* row : chosen) {
            total += row->*member;
        }
        return total;
    };

    const auto meetings = of({MEETING_KIND});
    const auto files = of({FILE_KIND});
    const auto walkthroughs = of({WALKTHROUGH_KIND});
    const auto actions = of({"action"});
    const auto commands = of({"command", "transform"});
    Counts targets;
    for (const Row* row : actions) {
        if (!row->target.empty()) {
            add(targets, row->target, 1);
        }
    }
    std::vector<const Row*> with_snippets;
    std::vector<const Row*> with_corrections;
    for (const Row* row : rows) {
        if (row->snippets != 0) {
            with_snippets.push_back(row);
        }
        if (row->corrections != 0) {
            with_corrections.push_back(row);
        }
    }

    std::vector<FeatureStats> result;
    FeatureStats meeting_stats = feature("meetings", meetings, once);
    meeting_stats.seconds = audio(meetings);
    meeting_stats.words = sum(meetings, &Row::final_words);
    result.push_back(std::move(meeting_stats));
    FeatureStats file_stats = feature("files", files, once);
    file_stats.seconds = audio(files);
    file_stats.words = sum(files, &Row::final_words);
    result.push_back(std::move(file_stats));
    FeatureStats walkthrough_stats = feature("walkthroughs", walkthroughs, once);
    walkthrough_stats.seconds = audio(walkthroughs);
    walkthrough_stats.shots = sum(walkthroughs, &Row::shots);
    result.push_back(std::move(walkthrough_stats));
    FeatureStats action_stats = feature("actions", actions, once);
    action_stats.top = most_first_folded(std::move(targets), TOP_TARGETS);
    result.push_back(std::move(action_stats));
    FeatureStats command_stats = feature("commands", commands, once);
    command_stats.words = sum(commands, &Row::edited);
    result.push_back(std::move(command_stats));
    FeatureStats snippet_stats =
        feature("snippets", with_snippets, [](const Row& row) { return row.snippets; });
    snippet_stats.words = sum(rows, &Row::snippet_words);
    result.push_back(std::move(snippet_stats));
    result.push_back(
        feature("dictionary", with_corrections, [](const Row& row) { return row.corrections; }));
    return result;
}

PeriodStats period(const std::vector<Row>& rows, const std::map<Day, std::int64_t>& by_day,
                   std::string_view key, std::optional<int> span, Day today) {
    PeriodStats stats;
    stats.key = std::string(key);
    stats.days = span;
    std::vector<const Row*> every;
    if (!span) {
        for (const Row& row : rows) {
            every.push_back(&row);
        }
        stats.bars = month_bars(by_day, today);
    } else {
        const Day start = plus_days(today, -(*span - 1));
        for (const Row& row : rows) {
            if (start <= row.day && row.day <= today) {
                every.push_back(&row);
            }
        }
        std::int64_t previous = 0;
        for (int back = 1; back <= *span; ++back) {
            previous += get(by_day, plus_days(start, -back));
        }
        stats.previous_words = previous;
        for (const Day day : days_back(today, *span)) {
            stats.bars.emplace_back(day, get(by_day, day));
        }
    }
    std::vector<const Row*> inside;
    for (const Row* row : every) {
        if (is_counted(row->kind)) {
            inside.push_back(row);
        }
    }
    for (const Row* row : inside) {
        stats.grid[static_cast<std::size_t>(weekday(row->day))]
                  [static_cast<std::size_t>(row->moment.hour)] += row->words;
        stats.words += row->words;
        stats.words_cleaned_up += row->edited;
        if (row->kind == "dictation") {
            ++stats.dictations;
        }
    }
    stats.rewrites = static_cast<std::int64_t>(inside.size()) - stats.dictations;
    stats.average_wpm = pace(inside);
    stats.top_apps = top_apps(inside);
    for (std::size_t day = 0; day < 7; ++day) {
        for (std::size_t hour = 0; hour < 24; ++hour) {
            stats.hours[hour] += stats.grid[day][hour];
            stats.weekdays[day] += stats.grid[day][hour];
        }
    }
    stats.categories = shares(inside, [](const Row& row) { return row.category; });
    stats.cleanup_levels = shares(inside, [](const Row& row) { return row.cleanup; });
    stats.languages = shares(inside, [](const Row& row) { return row.language; });
    stats.features = features(every, stats.bars, !span);
    return stats;
}

} // namespace

FeatureStats PeriodStats::feature(std::string_view wanted) const {
    for (const FeatureStats& found : features) {
        if (found.key == wanted) {
            return found;
        }
    }
    FeatureStats empty;
    empty.key = std::string(wanted);
    return empty;
}

double PeriodStats::minutes_saved() const {
    if (average_wpm <= TYPING_WPM) {
        return 0.0;
    }
    const auto spoken = static_cast<double>(words);
    return spoken / TYPING_WPM - spoken / average_wpm;
}

PeriodStats StatsSummary::period(std::string_view key) const {
    for (const PeriodStats& found : periods) {
        if (found.key == key) {
            return found;
        }
    }
    for (const auto& [name, span] : PERIODS) {
        if (name == key) {
            PeriodStats empty;
            empty.key = std::string(key);
            empty.days = span;
            return empty;
        }
    }
    throw py::PyError("KeyError", py::repr(key));
}

StatsSummary load_summary(StatsRepository& stats, const Clock& clock, std::optional<Day> today) {
    stats.ensure_backfilled();
    stats.count_features();
    const std::chrono::time_zone& zone = *clock.zone();
    const std::vector<Row> rows = read_rows(stats, zone);
    const Day local_today = today ? *today : day_of(history_detail::local_now(clock));
    const auto by_day = words_by_day(rows);

    StatsSummary summary;
    for (const Row& row : rows) {
        summary.words_all_time += row.words;
    }
    summary.entries = static_cast<std::int64_t>(rows.size());
    summary.day_streak = streak(by_day, local_today);
    summary.longest_streak = longest_streak(by_day);
    summary.dictated_today = by_day.contains(local_today);
    if (!by_day.empty()) {
        summary.first_day = by_day.begin()->first;
    }
    for (const Day day : days_back(local_today, ACTIVITY_DAYS)) {
        summary.activity.emplace_back(day, get(by_day, day));
    }
    for (const auto& [key, span] : PERIODS) {
        summary.periods.push_back(period(rows, by_day, key, span, local_today));
    }
    return summary;
}

} // namespace fv::dictation_stats
