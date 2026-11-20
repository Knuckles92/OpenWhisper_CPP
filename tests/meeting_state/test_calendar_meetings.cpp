// tests/test_history_calendar.py: the two tests that read meetings through the SQL repository
// (the History calendar's MeetingCalendarSource).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/testing/fake_clock.h"
#include "history/calendar.h"
#include "history/calendar_source.h"
#include "meeting_state/calendar_meetings.h"
#include "sql_support.h"

using fv::Json;
using namespace fv::meeting_state;
using fv::meeting_state::test_support::SqlFixture;

namespace {

void add_meeting(SqlMeetingRepository& repository, const std::string& meeting_id,
                 const std::string& started_at, const Json& ended_at = Json(),
                 const std::string& status = "ended", const std::string& title = "") {
    repository.create_meeting({{"id", meeting_id},
                               {"title", title},
                               {"status", status},
                               {"started_at", started_at},
                               {"ended_at", ended_at},
                               {"host_token", "host"},
                               {"guest_token", "guest"},
                               {"cloud_enabled", false},
                               {"spool_dir", ""}});
}

} // namespace

// from tests/test_history_calendar.py::test_source_reads_meetings_by_their_start
TEST_CASE("test_source_reads_meetings_by_their_start", "[test_history_calendar]") {
    SqlFixture db;
    add_meeting(*db.repo, "m_sep", "2026-09-10T16:00:00Z", "2026-09-10T16:30:00Z", "ended",
                "Standup");
    add_meeting(*db.repo, "m_edge", "2026-10-01T03:00:00Z", "2026-10-01T03:10:00Z");
    add_meeting(*db.repo, "m_live", "2026-09-12T16:00:00Z", Json(), "active");
    // Pacific daylight time, UTC-7: 03:00 UTC on October 1st is September 30th there.
    const fv::FakeClock clock(fv::FakeClock::default_utc(), "Etc/GMT+7");
    SqlMeetingCalendarSource meetings(db.repo, clock);
    fv::history::CalendarSource source(*db.database, clock, &meetings);

    std::vector<std::string> marked;
    double sep_seconds = -1;
    for (const auto& mark : source.load_index()) {
        if (mark.kind == fv::history::MEETING_KIND) {
            marked.push_back(mark.record_id);
            if (mark.record_id == "m_sep") {
                sep_seconds = mark.seconds;
            }
        }
    }
    std::sort(marked.begin(), marked.end());
    CHECK(marked == std::vector<std::string>{"m_edge", "m_sep"});
    CHECK(sep_seconds == 1800);

    const auto items = source.load_month(2026, 9);
    std::vector<std::pair<std::string, std::string>> listed;
    for (const auto& item : items) {
        listed.emplace_back(item.record_id, item.title);
    }
    CHECK(listed == std::vector<std::pair<std::string, std::string>>{
                        {"m_sep", "Standup"}, {"m_edge", "Untitled meeting"}});
    REQUIRE_FALSE(items.empty());
    CHECK(items[0].when == fv::py::DateTime{2026, 9, 10, 9, 0});
}

// from tests/test_history_calendar.py::test_meeting_summaries_can_be_bounded_by_start
TEST_CASE("test_meeting_summaries_can_be_bounded_by_start", "[test_history_calendar]") {
    SqlFixture db;
    const char* days[] = {"2026-08-31", "2026-09-05", "2026-10-02"};
    for (int index = 0; index < 3; ++index) {
        add_meeting(*db.repo, "m" + std::to_string(index), std::string(days[index]) + "T10:00:00Z");
    }
    SqlMeetingCalendarSource meetings(db.repo, db.clock);
    std::vector<std::string> bounded;
    for (const auto& row : meetings.list_past_meeting_summaries(101, "2026-09-01", "2026-10-01")) {
        bounded.push_back(row["id"].get<std::string>());
    }
    CHECK(bounded == std::vector<std::string>{"m1"});
    std::vector<std::string> times;
    for (const auto& row : meetings.list_past_meeting_times()) {
        times.push_back(row.id);
    }
    CHECK(times == std::vector<std::string>{"m0", "m1", "m2"});
}

TEST_CASE("calendar meetings: titles, previews and lengths come from meeting.content",
          "[meeting_state_calendar]") {
    SqlFixture db;
    SqlMeetingCalendarSource meetings(db.repo, db.clock);
    CHECK(meetings.fallback_meeting_title(Json{{"status", "failed"}}) == "Failed meeting");
    CHECK(meetings.fallback_meeting_title(Json{{"title", "Standup"}}) == "Standup");
    CHECK(meetings.meeting_preview_text(Json::object()) == "");
    CHECK(meetings.meeting_duration_s(Json{{"started_at", "2026-09-10T16:00:00Z"},
                                           {"ended_at", "2026-09-10T16:30:00Z"},
                                           {"paused_total_s", 600}}) == 1200.0);
    CHECK_FALSE(meetings.meeting_duration_s(Json::object()).has_value());
}
