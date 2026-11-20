#pragma once

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "base/json.h"
#include "history/entry.h"
#include "pyport/datetime.h"

// services/history_calendar.py, the pure half: what was recorded when, arranged by local day.
// Stored times mix aware UTC values with legacy naive local ones, so records are placed by local
// wall time, never by the stored string. The reads are calendar_source.h.
// Affinity: thread-safe (values and pure functions).
namespace fv::history {

// Record kinds (Python's DICTATION, FILE and MEETING).
inline constexpr std::string_view DICTATION_KIND = "dictation";
inline constexpr std::string_view FILE_KIND = "file";
inline constexpr std::string_view MEETING_KIND = "meeting";
inline constexpr std::array<std::string_view, 3> KINDS = {DICTATION_KIND, FILE_KIND, MEETING_KIND};
// "Dictation", "File", "Meeting"; "" for another kind.
std::string_view kind_label(std::string_view kind);
// "dictations", "files", "meetings"; "" for another kind.
std::string_view kind_plural(std::string_view kind);

// History's label for a Quick Record dictation, optionally followed by " · <profile>".
inline constexpr std::string_view QUICK_RECORD = "Quick Record";
// History's entry_kind for Command Mode and transform rewrites, and the prefix a transform's
// source_name carries.
inline constexpr std::string_view COMMAND_KIND = "command";
inline constexpr std::string_view TRANSFORM_KIND = "transform";
inline constexpr std::string_view TRANSFORM_PREFIX = "Transform \xC2\xB7 ";
// A dictation sent to a saved action; its source_name is "Action · <name>".
inline constexpr std::string_view ACTION_KIND = "action";
inline constexpr std::string_view ACTION_PREFIX = "Action \xC2\xB7 ";
// Spoken notes with screenshots.
inline constexpr std::string_view WALKTHROUGH_KIND = "walkthrough";
// Kinds whose label says what they are, not which profile or file.
inline constexpr std::array<std::string_view, 4> LABELLED_KINDS = {COMMAND_KIND, TRANSFORM_KIND,
                                                                   ACTION_KIND, WALKTHROUGH_KIND};

// Where a record is kept: recorded and kept here; kept here for a paired computer ("from"); or
// kept only on the paired host ("on"). A copy kept both here and on the host counts as here.
inline constexpr std::string_view HERE_SCOPE = "here";
inline constexpr std::string_view FROM = "from";
inline constexpr std::string_view ON = "on";

struct Place {
    std::string scope{HERE_SCOPE};
    std::string name;

    friend auto operator<=>(const Place&, const Place&) = default;
    friend bool operator==(const Place&, const Place&) = default;
};
// ("here", "").
const Place& here();

// A local day, and a calendar month as (year, month).
using Day = std::chrono::year_month_day;

struct MonthKey {
    int year = 1;
    int month = 1;

    friend auto operator<=>(const MonthKey&, const MonthKey&) = default;
    friend bool operator==(const MonthKey&, const MonthKey&) = default;
};

using KindSet = std::set<std::string, std::less<>>;

// A stored timestamp as naive local wall time in `zone`, or nullopt when empty or unreadable.
// The text is stripped and "Z" read as "+00:00"; aware values are converted, naive ones kept.
// Throws py::OverflowError("date value out of range") when the converted moment leaves years
// 1..9999.
std::optional<py::DateTime> local_wall_time(std::optional<std::string_view> value,
                                            const std::chrono::time_zone& zone);
// The same for a parsed datetime.
std::optional<py::DateTime> local_wall_time(const py::DateTime& value,
                                            const std::chrono::time_zone& zone);

// (kind, label) for a History entry's source_name and entry_kind (stripped and lower-cased).
// Command Mode and transform rewrites, actions and walkthroughs are live work and show as
// dictations labelled "Command", "Transform · <name>", "Action · <name>" or "Walkthrough".
// Older entries have only source_name: nothing or "Quick Record" with an optional profile name
// (the label) is a dictation; anything else names the transcribed file or files.
std::pair<std::string, std::string>
classify_source(std::optional<std::string_view> source_name,
                std::optional<std::string_view> entry_kind = std::nullopt);

// Where a History entry or a meeting (a JSON object) is kept: ("on", stored_on), else ("from",
// origin_device_name), else here.
Place record_place(const HistoryEntry& entry);
Place record_place(const Json& record);

// How a place reads in the Where filter: "Everywhere" for none, "From <name>", "Kept on <name>"
// or "This computer".
std::string place_label(const std::optional<Place>& place);

// Whether the ordinal-th recording ever is worth a star: 1, 10, 50, 100, 250, 500 and every
// thousand.
bool is_milestone(std::int64_t ordinal);
// "1st", "2nd", "11th", "1,000th".
std::string ordinal_suffix(std::int64_t number);
// "Your first recording", "Your 500th recording".
std::string milestone_text(std::int64_t ordinal);

MonthKey shift_month(MonthKey key, std::int64_t months);
// Months from a to b.
std::int64_t month_distance(MonthKey a, MonthKey b);
MonthKey month_key(Day day);
// The local day of a wall time.
Day day_of(const py::DateTime& when);

// Python's weekday numbering: 0 is Monday, 6 is Sunday.
inline constexpr int MONDAY = 0;
inline constexpr int SUNDAY = 6;
// The month's weeks as full rows of seven days, padded with the neighbours' days, starting on
// first_weekday (calendar.Calendar(first_weekday).monthdatescalendar). Throws py::ValueError as
// Python does for a month outside 1..12 or a padded day outside years 1..9999.
std::vector<std::vector<Day>> month_weeks(int year, int month, int first_weekday = SUNDAY);

// [start, end) "YYYY-MM-DD" bounds that hold every record of the month when compared as text
// with stored ISO timestamps. A stored UTC date is at most a day from its local date, so they
// are a day wider each side; callers then keep the local month exactly.
std::pair<std::string, std::string> query_bounds(int year, int month);

// One record in the index: when (naive local wall time), what kind, where, and how long.
struct Mark {
    py::DateTime when;
    std::string kind;
    Place place;
    std::string record_id;
    double seconds = 0.0;

    friend bool operator==(const Mark&, const Mark&) = default;
};

struct DaySummary {
    std::int64_t count = 0;
    double seconds = 0.0;
    std::map<std::string, std::int64_t, std::less<>> by_kind;

    friend bool operator==(const DaySummary&, const DaySummary&) = default;
};

struct MonthStats {
    std::int64_t count = 0;
    double seconds = 0.0;
    std::int64_t active_days = 0;
    // The earliest of the busiest days and its count.
    std::optional<std::pair<Day, std::int64_t>> busiest;
    std::map<std::string, std::int64_t, std::less<>> by_kind;

    friend bool operator==(const MonthStats&, const MonthStats&) = default;
};

// What a calendar item was made from: a History entry, or a meeting summary (a JSON object).
using CalendarRecord = std::variant<HistoryEntry, Json>;

// One record of the visible month, with what the calendar shows of it. `source` is what opening
// it needs.
struct CalendarItem {
    std::string kind;
    std::string record_id;
    py::DateTime when;
    std::string title;
    std::string preview;
    double seconds = 0.0;
    Place place;
    CalendarRecord source;
    std::string also_on;

    Day day() const { return day_of(when); }
    // The few words a day cell shows: a dictation's preview, else its title, else "Dictation";
    // any other record's title, else its preview, else its kind's label.
    std::string label() const;
    // (chip, tooltip) for where it is kept, or ("", "") for this computer alone.
    std::pair<std::string, std::string> location() const;
};

// Every record's day, kind and place: the calendar's counts, heat and stars. `kinds` and
// `place` narrow every count; nullopt means all of them.
class CalendarIndex {
public:
    CalendarIndex() = default;
    // Sorted by (when, record_id); milestones count every record, whatever the filters.
    explicit CalendarIndex(std::vector<Mark> marks);

    std::size_t size() const { return marks_.size(); }
    const std::vector<Mark>& marks() const { return marks_; }
    const std::set<Day>& active_days() const { return active_days_; }

    std::vector<Mark> select(const std::optional<KindSet>& kinds = std::nullopt,
                             const std::optional<Place>& place = std::nullopt) const;
    // The places records are kept: here first, then "from" before "on", each by name ignoring
    // case.
    std::vector<Place> places() const;
    std::map<MonthKey, std::int64_t>
    month_counts(const std::optional<KindSet>& kinds = std::nullopt,
                 const std::optional<Place>& place = std::nullopt) const;
    std::map<Day, DaySummary> days(int year, int month,
                                   const std::optional<KindSet>& kinds = std::nullopt,
                                   const std::optional<Place>& place = std::nullopt) const;
    MonthStats month_stats(int year, int month, const std::optional<KindSet>& kinds = std::nullopt,
                           const std::optional<Place>& place = std::nullopt) const;
    // Days in a row with a recording, up to today. A streak stays alive through today before
    // anything is recorded, so it counts back from yesterday until today has a recording.
    std::int64_t streak(Day today) const;
    // The record's place among every recording ever, when it is a milestone.
    std::optional<std::int64_t> milestone(std::string_view record_id) const;
    // The month's days holding a milestone, with the largest one on each.
    std::map<Day, std::int64_t> milestone_days(int year, int month) const;
    // The month with records closest to `key`, the earlier one on a tie.
    std::optional<MonthKey> nearest_month(MonthKey key,
                                          const std::optional<KindSet>& kinds = std::nullopt,
                                          const std::optional<Place>& place = std::nullopt) const;

private:
    std::vector<Mark> marks_;
    std::map<std::string, std::int64_t, std::less<>> milestones_;
    std::set<Day> active_days_;
};

} // namespace fv::history
