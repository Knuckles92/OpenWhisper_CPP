#include "history/calendar.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <tuple>

#include "history/detail/py_value.h"
#include "history/detail/time.h"
#include "history/detail/time_range.h"
#include "pyport/errors.h"
#include "pyport/text.h"

namespace fv::history {

namespace {

using std::chrono::days;
using std::chrono::sys_days;

// calendar.IllegalMonthError, a ValueError.
class IllegalMonthError : public py::ValueError {
public:
    explicit IllegalMonthError(int month)
        : py::ValueError("IllegalMonthError",
                         std::format("bad month number {}; must be 1-12", month)) {}
};

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

std::int64_t floor_mod(std::int64_t a, std::int64_t b) {
    return a - floor_div(a, b) * b;
}

auto wall_key(const py::DateTime& moment) {
    return std::make_tuple(moment.year, moment.month, moment.day, moment.hour, moment.minute,
                           moment.second, moment.microsecond);
}

Day make_day(int year, int month, int day) {
    return Day{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
               std::chrono::day{static_cast<unsigned>(day)}};
}

int year_of(Day day) {
    return static_cast<int>(day.year());
}

int month_of(Day day) {
    return static_cast<int>(static_cast<unsigned>(day.month()));
}

// date(year, month, 1): Python's checks, in its order.
void check_date(int year, int month) {
    if (year < 1 || year > 9999) {
        throw py::ValueError(std::format("year {} is out of range", year));
    }
    if (month < 1 || month > 12) {
        throw py::ValueError("month must be in 1..12");
    }
}

std::string iso_day(Day day) {
    return std::format("{:04}-{:02}-{:02}", year_of(day), month_of(day),
                       static_cast<unsigned>(day.day()));
}

bool matches(const Mark& mark, const std::optional<KindSet>& kinds,
             const std::optional<Place>& place) {
    return (!kinds || kinds->contains(mark.kind)) && (!place || mark.place == *place);
}

} // namespace

std::string_view kind_label(std::string_view kind) {
    if (kind == DICTATION_KIND) {
        return "Dictation";
    }
    if (kind == FILE_KIND) {
        return "File";
    }
    if (kind == MEETING_KIND) {
        return "Meeting";
    }
    return "";
}

std::string_view kind_plural(std::string_view kind) {
    if (kind == DICTATION_KIND) {
        return "dictations";
    }
    if (kind == FILE_KIND) {
        return "files";
    }
    if (kind == MEETING_KIND) {
        return "meetings";
    }
    return "";
}

const Place& here() {
    static const Place place{};
    return place;
}

std::optional<py::DateTime> local_wall_time(std::optional<std::string_view> value,
                                            const std::chrono::time_zone& zone) {
    if (!value || value->empty()) {
        return std::nullopt;
    }
    std::string text = py::strip(*value);
    for (std::size_t at = text.find('Z'); at != std::string::npos; at = text.find('Z', at)) {
        text.replace(at, 1, "+00:00");
        at += 6;
    }
    py::DateTime parsed;
    try {
        parsed = py::fromisoformat(text);
    } catch (const py::ValueError&) {
        return std::nullopt;
    }
    return local_wall_time(parsed, zone);
}

std::optional<py::DateTime> local_wall_time(const py::DateTime& value,
                                            const std::chrono::time_zone& zone) {
    if (!value.utc_offset_us) {
        return value;
    }
    const auto instant = history_detail::utc_moment(value);
    if (!history_detail::utc_in_range(instant)) {
        throw py::OverflowError("date value out of range");
    }
    const auto local = history_detail::to_zone(instant, zone);
    if (!local) {
        throw py::OverflowError("date value out of range");
    }
    return local;
}

std::pair<std::string, std::string> classify_source(std::optional<std::string_view> source_name,
                                                    std::optional<std::string_view> entry_kind) {
    const std::string name = py::strip(source_name.value_or(""));
    // A host's listing may come from an older or newer version.
    const std::string kind = entry_kind ? py::lower(py::strip(*entry_kind)) : std::string();
    const std::string dictation(DICTATION_KIND);
    if (kind == COMMAND_KIND) {
        return {dictation, "Command"};
    }
    if (kind == WALKTHROUGH_KIND) {
        return {dictation, "Walkthrough"};
    }
    const auto named = [&](std::string_view prefix, std::string_view bare) {
        std::string_view rest = name;
        if (rest.starts_with(prefix)) {
            rest.remove_prefix(prefix.size());
        }
        const std::string what = py::strip(rest);
        return std::pair<std::string, std::string>{
            dictation, what.empty() ? std::string(bare) : std::string(prefix) + what};
    };
    if (kind == TRANSFORM_KIND) {
        return named(TRANSFORM_PREFIX, "Transform");
    }
    if (kind == ACTION_KIND) {
        return named(ACTION_PREFIX, "Action");
    }
    if (kind == FILE_KIND) {
        return {std::string(FILE_KIND), name};
    }
    if (name.empty()) {
        return {dictation, ""};
    }
    if (name == QUICK_RECORD || name.starts_with(std::string(QUICK_RECORD) + " \xC2\xB7")) {
        return {dictation, py::strip(py::lstrip(std::string_view(name).substr(QUICK_RECORD.size()),
                                                " \xC2\xB7"))};
    }
    return {kind == DICTATION_KIND ? dictation : std::string(FILE_KIND), name};
}

Place record_place(const HistoryEntry& entry) {
    if (std::string stored_on = py::strip(entry.stored_on.value_or("")); !stored_on.empty()) {
        return {std::string(ON), std::move(stored_on)};
    }
    if (std::string origin = py::strip(entry.origin_device_name.value_or("")); !origin.empty()) {
        return {std::string(FROM), std::move(origin)};
    }
    return here();
}

Place record_place(const Json& record) {
    const auto read = [&](std::string_view name) {
        return py::strip(history_detail::text_or(record, name, ""));
    };
    if (std::string stored_on = read("stored_on"); !stored_on.empty()) {
        return {std::string(ON), std::move(stored_on)};
    }
    if (std::string origin = read("origin_device_name"); !origin.empty()) {
        return {std::string(FROM), std::move(origin)};
    }
    return here();
}

std::string place_label(const std::optional<Place>& place) {
    if (!place) {
        return "Everywhere";
    }
    if (place->scope == FROM) {
        return "From " + place->name;
    }
    if (place->scope == ON) {
        return "Kept on " + place->name;
    }
    return "This computer";
}

bool is_milestone(std::int64_t ordinal) {
    switch (ordinal) {
    case 1:
    case 10:
    case 50:
    case 100:
    case 250:
    case 500:
        return true;
    default:
        return ordinal > 0 && ordinal % 1000 == 0;
    }
}

std::string ordinal_suffix(std::int64_t number) {
    const std::int64_t tens = floor_mod(number, 100);
    std::string_view suffix = "th";
    if (tens < 10 || tens > 20) {
        switch (floor_mod(number, 10)) {
        case 1:
            suffix = "st";
            break;
        case 2:
            suffix = "nd";
            break;
        case 3:
            suffix = "rd";
            break;
        default:
            break;
        }
    }
    // f"{number:,}": thousands separated by commas.
    const auto magnitude = static_cast<std::uint64_t>(number);
    const std::string digits =
        std::to_string(number < 0 ? std::uint64_t{0} - magnitude : magnitude);
    std::string grouped;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) {
            grouped += ',';
        }
        grouped += digits[i];
    }
    return (number < 0 ? "-" : "") + grouped + std::string(suffix);
}

std::string milestone_text(std::int64_t ordinal) {
    return ordinal == 1 ? std::string("Your first recording")
                        : "Your " + ordinal_suffix(ordinal) + " recording";
}

MonthKey shift_month(MonthKey key, std::int64_t months) {
    const std::int64_t index = static_cast<std::int64_t>(key.year) * 12 + (key.month - 1) + months;
    return {static_cast<int>(floor_div(index, 12)), static_cast<int>(floor_mod(index, 12) + 1)};
}

std::int64_t month_distance(MonthKey a, MonthKey b) {
    return (static_cast<std::int64_t>(b.year) * 12 + b.month) -
           (static_cast<std::int64_t>(a.year) * 12 + a.month);
}

MonthKey month_key(Day day) {
    return {year_of(day), month_of(day)};
}

Day day_of(const py::DateTime& when) {
    return make_day(when.year, when.month, when.day);
}

std::vector<std::vector<Day>> month_weeks(int year, int month, int first_weekday) {
    if (month < 1 || month > 12) {
        throw IllegalMonthError(month);
    }
    const Day first = make_day(year, month, 1);
    const Day last{first.year() / first.month() / std::chrono::last};
    // Python numbers weekdays from Monday (0); ISO from Monday (1).
    const int day1 = static_cast<int>(std::chrono::weekday{sys_days{first}}.iso_encoding()) - 1;
    const int count = static_cast<int>(static_cast<unsigned>(last.day()));
    const int before = static_cast<int>(floor_mod(day1 - first_weekday, 7));
    const int after = static_cast<int>(floor_mod(first_weekday - day1 - count, 7));
    std::vector<std::vector<Day>> weeks;
    const sys_days start = sys_days{first} - days{before};
    const int total = before + count + after;
    for (int offset = 0; offset < total; ++offset) {
        const Day day{start + days{offset}};
        if (day.year() < std::chrono::year{1} || day.year() > std::chrono::year{9999}) {
            throw py::ValueError(std::format("year {} is out of range", year_of(day)));
        }
        if (offset % 7 == 0) {
            weeks.emplace_back();
        }
        weeks.back().push_back(day);
    }
    return weeks;
}

std::pair<std::string, std::string> query_bounds(int year, int month) {
    check_date(year, month);
    const Day first = make_day(year, month, 1);
    const Day last{first.year() / first.month() / std::chrono::last};
    const Day start{sys_days{first} - days{1}};
    const Day end{sys_days{last} + days{2}};
    if (start.year() < std::chrono::year{1} || end.year() > std::chrono::year{9999}) {
        throw py::OverflowError("date value out of range");
    }
    return {iso_day(start), iso_day(end)};
}

std::string CalendarItem::label() const {
    if (kind == DICTATION_KIND) {
        if (!preview.empty()) {
            return preview;
        }
        return title.empty() ? std::string("Dictation") : title;
    }
    if (!title.empty()) {
        return title;
    }
    return preview.empty() ? std::string(kind_label(kind)) : preview;
}

std::pair<std::string, std::string> CalendarItem::location() const {
    if (place.scope == ON) {
        return {"On " + place.name, "Kept on " + place.name + "; opened from there"};
    }
    if (!also_on.empty()) {
        return {"Also on " + also_on, "Kept here and on " + also_on};
    }
    if (place.scope == FROM) {
        return {"From " + place.name, "Kept here for " + place.name + ", a paired computer"};
    }
    return {"", ""};
}

CalendarIndex::CalendarIndex(std::vector<Mark> marks) : marks_(std::move(marks)) {
    std::ranges::stable_sort(marks_, [](const Mark& a, const Mark& b) {
        const auto ka = wall_key(a.when);
        const auto kb = wall_key(b.when);
        return ka != kb ? ka < kb : a.record_id < b.record_id;
    });
    std::int64_t ordinal = 0;
    for (const Mark& mark : marks_) {
        ++ordinal;
        if (is_milestone(ordinal)) {
            milestones_[mark.record_id] = ordinal;
        }
        active_days_.insert(day_of(mark.when));
    }
}

std::vector<Mark> CalendarIndex::select(const std::optional<KindSet>& kinds,
                                        const std::optional<Place>& place) const {
    if (!kinds && !place) {
        return marks_;
    }
    std::vector<Mark> chosen;
    for (const Mark& mark : marks_) {
        if (matches(mark, kinds, place)) {
            chosen.push_back(mark);
        }
    }
    return chosen;
}

std::vector<Place> CalendarIndex::places() const {
    std::set<Place> found;
    for (const Mark& mark : marks_) {
        found.insert(mark.place);
    }
    std::vector<Place> others;
    for (const Place& place : found) {
        if (place != here()) {
            others.push_back(place);
        }
    }
    // Python sorts by (scope != "from", name.lower()); a tie there keeps a set's arbitrary
    // order, here the places' own order.
    std::ranges::stable_sort(others, [](const Place& a, const Place& b) {
        return std::make_tuple(a.scope != FROM, py::lower(a.name)) <
               std::make_tuple(b.scope != FROM, py::lower(b.name));
    });
    std::vector<Place> result;
    if (found.contains(here())) {
        result.push_back(here());
    }
    result.insert(result.end(), others.begin(), others.end());
    return result;
}

std::map<MonthKey, std::int64_t>
CalendarIndex::month_counts(const std::optional<KindSet>& kinds,
                            const std::optional<Place>& place) const {
    std::map<MonthKey, std::int64_t> counts;
    for (const Mark& mark : marks_) {
        if (matches(mark, kinds, place)) {
            ++counts[month_key(day_of(mark.when))];
        }
    }
    return counts;
}

std::map<Day, DaySummary> CalendarIndex::days(int year, int month,
                                              const std::optional<KindSet>& kinds,
                                              const std::optional<Place>& place) const {
    std::map<Day, DaySummary> result;
    for (const Mark& mark : marks_) {
        if (!matches(mark, kinds, place) || mark.when.year != year || mark.when.month != month) {
            continue;
        }
        DaySummary& summary = result[day_of(mark.when)];
        summary.count += 1;
        summary.seconds += mark.seconds;
        summary.by_kind[mark.kind] += 1;
    }
    return result;
}

MonthStats CalendarIndex::month_stats(int year, int month, const std::optional<KindSet>& kinds,
                                      const std::optional<Place>& place) const {
    const auto found = days(year, month, kinds, place);
    MonthStats stats;
    stats.active_days = static_cast<std::int64_t>(found.size());
    for (const auto& [day, summary] : found) {
        stats.count += summary.count;
        stats.seconds += summary.seconds;
        for (const auto& [kind, count] : summary.by_kind) {
            stats.by_kind[kind] += count;
        }
        if (!stats.busiest || summary.count > stats.busiest->second ||
            (summary.count == stats.busiest->second && day < stats.busiest->first)) {
            stats.busiest = std::make_pair(day, summary.count);
        }
    }
    return stats;
}

std::int64_t CalendarIndex::streak(Day today) const {
    sys_days day = sys_days{today};
    if (!active_days_.contains(today)) {
        day -= std::chrono::days{1};
    }
    std::int64_t count = 0;
    while (active_days_.contains(Day{day})) {
        ++count;
        day -= std::chrono::days{1};
    }
    return count;
}

std::optional<std::int64_t> CalendarIndex::milestone(std::string_view record_id) const {
    const auto found = milestones_.find(record_id);
    if (found == milestones_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::map<Day, std::int64_t> CalendarIndex::milestone_days(int year, int month) const {
    std::map<Day, std::int64_t> result;
    for (const Mark& mark : marks_) {
        const auto ordinal = milestone(mark.record_id);
        if (ordinal && mark.when.year == year && mark.when.month == month) {
            std::int64_t& largest = result[day_of(mark.when)];
            largest = std::max(largest, *ordinal);
        }
    }
    return result;
}

std::optional<MonthKey> CalendarIndex::nearest_month(MonthKey key,
                                                     const std::optional<KindSet>& kinds,
                                                     const std::optional<Place>& place) const {
    std::optional<MonthKey> best;
    std::pair<std::int64_t, bool> best_rank{};
    for (const auto& [month, _count] : month_counts(kinds, place)) {
        const std::int64_t distance = month_distance(key, month);
        const std::pair<std::int64_t, bool> rank{std::llabs(distance), distance > 0};
        if (!best || rank < best_rank) {
            best = month;
            best_rank = rank;
        }
    }
    return best;
}

} // namespace fv::history
