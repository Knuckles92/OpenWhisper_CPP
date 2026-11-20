#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "history/calendar_source.h"
#include "meeting_state/sql_repository.h"

// services/history_calendar.py's meeting reads: the History calendar's past meetings come from
// the SQL meeting repository and meeting.content, as CalendarSource._meetings() and
// meeting_item read them.
// Affinity: as the repository (thread-safe); blocking, never on the GUI thread.
namespace fv::meeting_state {

class SqlMeetingCalendarSource final : public history::MeetingCalendarSource {
public:
    // `clock` reads naive stored times as local wall time when it measures a meeting; it must
    // outlive the source.
    explicit SqlMeetingCalendarSource(std::shared_ptr<SqlMeetingRepository> repository,
                                      const Clock& clock = system_clock());

    std::vector<history::MeetingTimeRow> list_past_meeting_times() override;
    std::vector<Json> list_past_meeting_summaries(int limit, std::string_view started_from,
                                                  std::string_view started_before) override;
    std::string fallback_meeting_title(const Json& meeting) override;
    std::string meeting_preview_text(const Json& meeting) override;
    std::optional<double> meeting_duration_s(const Json& meeting) override;

private:
    std::shared_ptr<SqlMeetingRepository> repository_;
    const Clock& clock_;
};

} // namespace fv::meeting_state
