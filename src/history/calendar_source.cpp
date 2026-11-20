#include "history/calendar_source.h"

#include <algorithm>
#include <exception>
#include <set>
#include <tuple>
#include <utility>

#include "base/log.h"
#include "history/detail/py_value.h"
#include "history/repository.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::history {

namespace {

constexpr log::Logger logger("services.history_calendar");

// Past meeting summaries one month asks for (one more than the host lists).
constexpr int kMonthMeetingLimit = 501;

using history_detail::field;
using history_detail::py_str;
using history_detail::truthy;

// local_wall_time(record.get(key), tz) over a dict value.
std::optional<py::DateTime> wall_time_of(const Json& value, const std::chrono::time_zone& zone) {
    if (!truthy(value)) {
        return std::nullopt;
    }
    return local_wall_time(std::optional<std::string_view>(py_str(value)), zone);
}

bool in_month(const std::optional<py::DateTime>& when, int year, int month) {
    return when && when->year == year && when->month == month;
}

std::optional<std::string> optional_text(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    return py_str(value);
}

// float(value or 0.0).
double seconds_of(const Json& value) {
    return truthy(value) ? py::py_float(value).value_or(0.0) : 0.0;
}

std::string meeting_id(const Json& meeting) {
    return history_detail::text_or(meeting, "id", "");
}

double meeting_seconds(MeetingCalendarSource& meetings, const Json& meeting) {
    return meetings.meeting_duration_s(meeting).value_or(0.0);
}

// Stateless, so one instance serves every source on any thread.
MeetingCalendarSource& no_meetings() {
    static NullMeetingCalendarSource none;
    return none;
}

RemoteRecordSource& no_records() {
    static NullRemoteRecordSource none;
    return none;
}

} // namespace

CalendarItem entry_item(const HistoryEntry& entry, const py::DateTime& when, std::string also_on) {
    auto [kind, label] = classify_source(entry.source_name, entry.entry_kind);
    CalendarItem item;
    item.kind = std::move(kind);
    item.record_id = entry.id;
    item.when = when;
    item.title = py::strip(entry.title.value_or(""));
    if (item.title.empty()) {
        item.title = std::move(label);
    }
    item.preview = py::strip(entry.text).empty() ? std::string() : entry.preview_text();
    item.seconds = entry.audio_duration.value_or(0.0);
    item.place = record_place(entry);
    item.source = entry;
    item.also_on = std::move(also_on);
    return item;
}

CalendarItem meeting_item(MeetingCalendarSource& meetings, const Json& meeting,
                          const py::DateTime& when, std::string also_on) {
    CalendarItem item;
    item.kind = std::string(MEETING_KIND);
    item.record_id = meeting_id(meeting);
    item.when = when;
    item.title = meetings.fallback_meeting_title(meeting);
    item.preview = meetings.meeting_preview_text(meeting);
    item.seconds = meeting_seconds(meetings, meeting);
    item.place = record_place(meeting);
    item.source = meeting;
    item.also_on = std::move(also_on);
    return item;
}

CalendarSource::CalendarSource(Database& database, const Clock& clock,
                               MeetingCalendarSource* meetings, RemoteRecordSource* records)
    : database_(&database), clock_(&clock),
      meetings_(meetings != nullptr ? meetings : &no_meetings()),
      records_(records != nullptr ? records : &no_records()) {}

std::vector<Mark> CalendarSource::load_index() {
    std::vector<Mark> marks;
    HistoryRepository repository(*database_);
    for (const HistoryCalendarRow& row : repository.history_calendar_rows()) {
        const auto when = local_wall_time(std::optional<std::string_view>(row.timestamp), zone());
        if (!when) {
            continue;
        }
        auto [kind, _label] = classify_source(row.source_name, row.entry_kind);
        const bool paired = row.origin_device_name && !row.origin_device_name->empty();
        marks.push_back({*when, std::move(kind),
                         paired ? Place{std::string(FROM), *row.origin_device_name} : here(),
                         row.id, row.audio_duration.value_or(0.0)});
    }
    for (const MeetingTimeRow& row : meetings_->list_past_meeting_times()) {
        const auto when = local_wall_time(
            row.started_at ? std::optional<std::string_view>(*row.started_at) : std::nullopt,
            zone());
        if (!when) {
            continue;
        }
        const bool paired = row.origin_device_name && !row.origin_device_name->empty();
        Json times = Json::object();
        times["started_at"] = row.started_at ? Json(*row.started_at) : Json(nullptr);
        times["ended_at"] = row.ended_at ? Json(*row.ended_at) : Json(nullptr);
        times["paused_total_s"] = row.paused_total_s ? Json(*row.paused_total_s) : Json(nullptr);
        marks.push_back({*when, std::string(MEETING_KIND),
                         paired ? Place{std::string(FROM), *row.origin_device_name} : here(),
                         row.id, meeting_seconds(*meetings_, times)});
    }
    return marks;
}

bool CalendarSource::remote_wanted() {
    try {
        return records_->listing_wanted(DICTATION_KIND) || records_->listing_wanted(MEETING_KIND);
    } catch (const std::exception&) {
        logger.debug("Record sync unavailable for the History calendar");
        return false;
    }
}

std::vector<Mark> CalendarSource::load_remote(const std::vector<std::string>& local_ids) {
    notice_.clear();
    std::vector<Json> dictations;
    std::vector<Json> meetings;
    try {
        if (records_->listing_wanted(DICTATION_KIND)) {
            dictations = records_->list_remote(DICTATION_KIND, "", REMOTE_LIMIT);
        }
        if (records_->listing_wanted(MEETING_KIND)) {
            meetings = records_->list_remote(MEETING_KIND, "", REMOTE_LIMIT);
        }
    } catch (const std::exception& error) {
        logger.debug("Couldn't list the host's records for the calendar: {}", error.what());
        notice_ = error.what();
        if (notice_.empty()) {
            notice_ = "The host couldn't be asked.";
        }
    }
    const std::set<std::string, std::less<>> known(local_ids.begin(), local_ids.end());
    const auto unknown = [&](std::vector<Json> items) {
        std::erase_if(items, [&](const Json& item) {
            return known.contains(history_detail::text_or(item, "id", ""));
        });
        return items;
    };
    remote_dictations_ = unknown(std::move(dictations));
    remote_meetings_ = unknown(std::move(meetings));

    std::vector<Mark> marks;
    for (const Json& item : remote_dictations_) {
        if (const auto when = wall_time_of(field(item, "timestamp"), zone())) {
            const auto source_name = optional_text(field(item, "source_name"));
            const Json& kind_value = field(item, "entry_kind");
            // classify_source ignores an entry_kind that is not text.
            const auto entry_kind = kind_value.is_string()
                                        ? std::optional<std::string>(kind_value.get<std::string>())
                                        : std::nullopt;
            auto [kind, _label] = classify_source(source_name, entry_kind);
            marks.push_back({*when, std::move(kind), record_place(item), py_str(field(item, "id")),
                             seconds_of(field(item, "audio_duration"))});
        }
    }
    for (const Json& item : remote_meetings_) {
        if (const auto when = wall_time_of(field(item, "started_at"), zone())) {
            marks.push_back({*when, std::string(MEETING_KIND), record_place(item),
                             py_str(field(item, "id")), meeting_seconds(*meetings_, item)});
        }
    }
    return marks;
}

std::vector<CalendarItem> CalendarSource::load_month(int year, int month) {
    const auto [start, end] = query_bounds(year, month);
    std::vector<CalendarItem> items = month_dictations(year, month, start, end);
    std::vector<CalendarItem> meetings = month_meetings(year, month, start, end);
    items.insert(items.end(), std::make_move_iterator(meetings.begin()),
                 std::make_move_iterator(meetings.end()));
    std::ranges::stable_sort(items, [](const CalendarItem& a, const CalendarItem& b) {
        const auto key = [](const CalendarItem& item) {
            const py::DateTime& w = item.when;
            return std::make_tuple(w.year, w.month, w.day, w.hour, w.minute, w.second,
                                   w.microsecond);
        };
        const auto ka = key(a);
        const auto kb = key(b);
        return ka != kb ? ka < kb : a.record_id < b.record_id;
    });
    return items;
}

std::vector<CalendarItem> CalendarSource::month_dictations(int year, int month,
                                                           const std::string& start,
                                                           const std::string& end) {
    std::vector<std::pair<HistoryEntry, py::DateTime>> entries;
    HistoryRepository repository(*database_);
    for (HistoryEntry& entry : repository.get_history_entries_between(start, end)) {
        const auto when = local_wall_time(std::optional<std::string_view>(entry.timestamp), zone());
        if (in_month(when, year, month)) {
            entries.emplace_back(std::move(entry), *when);
        }
    }
    std::vector<std::string> ids;
    for (const auto& [entry, _when] : entries) {
        ids.push_back(entry.id);
    }
    const auto also_on = copies_on_host(DICTATION_KIND, ids);
    std::vector<CalendarItem> items;
    for (const auto& [entry, when] : entries) {
        const auto copy = also_on.find(entry.id);
        items.push_back(entry_item(entry, when, copy == also_on.end() ? "" : copy->second));
    }
    for (const Json& item : remote_dictations_) {
        const auto when = wall_time_of(field(item, "timestamp"), zone());
        if (in_month(when, year, month)) {
            items.push_back(entry_item(records_->remote_history_entry(item), *when));
        }
    }
    return items;
}

std::vector<CalendarItem> CalendarSource::month_meetings(int year, int month,
                                                         const std::string& start,
                                                         const std::string& end) {
    std::vector<std::pair<Json, py::DateTime>> found;
    for (Json& meeting : meetings_->list_past_meeting_summaries(kMonthMeetingLimit, start, end)) {
        const auto when = wall_time_of(field(meeting, "started_at"), zone());
        if (in_month(when, year, month)) {
            found.emplace_back(std::move(meeting), *when);
        }
    }
    std::vector<std::string> ids;
    for (const auto& [meeting, _when] : found) {
        ids.push_back(meeting_id(meeting));
    }
    const auto also_on = copies_on_host(MEETING_KIND, ids);
    std::vector<CalendarItem> items;
    for (const auto& [meeting, when] : found) {
        const auto copy = also_on.find(meeting_id(meeting));
        items.push_back(
            meeting_item(*meetings_, meeting, when, copy == also_on.end() ? "" : copy->second));
    }
    for (const Json& meeting : remote_meetings_) {
        const auto when = wall_time_of(field(meeting, "started_at"), zone());
        if (in_month(when, year, month)) {
            items.push_back(meeting_item(*meetings_, meeting, *when));
        }
    }
    return items;
}

std::map<std::string, std::string>
CalendarSource::copies_on_host(std::string_view kind, const std::vector<std::string>& ids) {
    if (ids.empty()) {
        return {};
    }
    std::vector<std::string> copies;
    std::string host;
    try {
        copies = records_->copies_on_host(kind, ids);
        if (copies.empty()) {
            return {};
        }
        host = records_->host_name();
        if (host.empty()) {
            host = "the host";
        }
    } catch (const std::exception&) {
        logger.debug("Couldn't tell which records the host also keeps");
        return {};
    }
    std::map<std::string, std::string> result;
    for (std::string& id : copies) {
        result.emplace(std::move(id), host);
    }
    return result;
}

} // namespace fv::history
