#include "meeting_state/calendar_meetings.h"

#include <utility>

#include "meeting_state/content.h"
#include "meeting_state/time_utils.h"

namespace fv::meeting_state {

SqlMeetingCalendarSource::SqlMeetingCalendarSource(std::shared_ptr<SqlMeetingRepository> repository,
                                                   const Clock& clock)
    : repository_(std::move(repository)), clock_(clock) {}

std::vector<history::MeetingTimeRow> SqlMeetingCalendarSource::list_past_meeting_times() {
    std::vector<history::MeetingTimeRow> rows;
    for (auto& time : repository_->list_past_meeting_times()) {
        history::MeetingTimeRow row;
        row.id = std::move(time.id);
        row.started_at = std::move(time.started_at);
        row.ended_at = std::move(time.ended_at);
        row.paused_total_s = time.paused_total_s;
        row.origin_device_name = std::move(time.origin_device_name);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<Json>
SqlMeetingCalendarSource::list_past_meeting_summaries(int limit, std::string_view started_from,
                                                      std::string_view started_before) {
    PastMeetingQuery query;
    query.limit = limit;
    query.started_from = std::string(started_from);
    query.started_before = std::string(started_before);
    return repository_->list_past_meeting_summaries(query);
}

std::string SqlMeetingCalendarSource::fallback_meeting_title(const Json& meeting) {
    return meeting_state::fallback_meeting_title(meeting);
}

std::string SqlMeetingCalendarSource::meeting_preview_text(const Json& meeting) {
    return meeting_state::meeting_preview_text(meeting);
}

std::optional<double> SqlMeetingCalendarSource::meeting_duration_s(const Json& meeting) {
    return meeting_state::meeting_duration_s(meeting, false, clock_);
}

} // namespace fv::meeting_state
