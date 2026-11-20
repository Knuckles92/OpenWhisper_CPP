// tests/test_meeting_time_utils.py: UTC storage and local display of Meeting Mode timestamps.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "base/testing/fake_clock.h"
#include "meeting_state/time_utils.h"
#include "pyport/datetime.h"

using fv::Json;
using namespace fv::meeting_state;
using namespace std::chrono_literals;

namespace {

std::chrono::sys_seconds utc(int year, unsigned month, unsigned day, int hour = 0, int minute = 0,
                             int second = 0) {
    return std::chrono::sys_days{std::chrono::year{year} / month / day} + std::chrono::hours{hour} +
           std::chrono::minutes{minute} + std::chrono::seconds{second};
}

} // namespace

// from tests/test_meeting_time_utils.py::test_utc_timestamp_converts_to_requested_local_timezone
TEST_CASE("test_utc_timestamp_converts_to_requested_local_timezone", "[test_meeting_time_utils]") {
    const auto local = as_local_time("2026-08-20T16:55:00Z", std::chrono::seconds(-7h));

    REQUIRE(local.has_value());
    // strftime("%Y-%m-%d %I:%M %p") == "2026-08-20 09:55 AM"
    CHECK(local->isoformat() == "2026-08-20T09:55:00-07:00");
}

// from tests/test_meeting_time_utils.py::test_legacy_naive_timestamp_keeps_its_wall_time
TEST_CASE("test_legacy_naive_timestamp_keeps_its_wall_time", "[test_meeting_time_utils]") {
    const auto legacy = as_local_time("2026-08-20T09:55:00", std::chrono::seconds(-4h));

    REQUIRE(legacy.has_value());
    CHECK(*legacy == fv::py::DateTime{2026, 8, 20, 9, 55, 0, 0, std::nullopt});
    CHECK_FALSE(legacy->utc_offset_us.has_value());
}

// from tests/test_meeting_time_utils.py::test_new_timestamps_are_aware_utc
TEST_CASE("test_new_timestamps_are_aware_utc", "[test_meeting_time_utils]") {
    const std::string value = utc_now_iso();
    const auto parsed = parse_meeting_time(value);

    CHECK(value.ends_with("Z"));
    REQUIRE(parsed.has_value());
    CHECK(parsed->utc_offset_us == 0);
}

// from tests/test_meeting_time_utils.py::test_elapsed_seconds_handles_aware_and_legacy_pairs
TEST_CASE("test_elapsed_seconds_handles_aware_and_legacy_pairs", "[test_meeting_time_utils]") {
    CHECK(elapsed_seconds("2026-08-20T16:55:00Z", "2026-08-20T16:55:40Z") == 40.0);
    CHECK(elapsed_seconds("2026-08-20T09:55:00", "2026-08-20T09:55:40") == 40.0);
}

// from tests/test_meeting_time_utils.py::test_meeting_duration_counts_a_running_meeting_up_to_now
TEST_CASE("test_meeting_duration_counts_a_running_meeting_up_to_now", "[test_meeting_time_utils]") {
    const fv::FakeClock clock(utc(2026, 9, 1, 12), "America/New_York");
    // (datetime.now(timezone.utc) - timedelta(minutes=10)).isoformat()
    Json live = {
        {"started_at", "2026-09-01T11:50:00+00:00"}, {"ended_at", nullptr}, {"paused_total_s", 60}};

    const auto running = meeting_duration_s(live, true, clock);
    REQUIRE(running.has_value());
    CHECK(*running >= 535.0);
    CHECK(*running <= 545.0);
    CHECK_FALSE(meeting_duration_s(live, false, clock).has_value());
    Json ended = live;
    ended["ended_at"] = "2026-08-20T16:55:40Z";
    ended["started_at"] = "2026-08-20T16:50:00Z";
    CHECK(meeting_duration_s(ended, true, clock) == 340.0 - 60.0);
}

TEST_CASE("a mixed aware and legacy pair reads the naive side as local wall time",
          "[test_meeting_time_utils]") {
    // 09:55 New York daylight time is 13:55 UTC.
    const fv::FakeClock clock(utc(2026, 8, 20), "America/New_York");
    CHECK(elapsed_seconds("2026-08-20T09:55:00", "2026-08-20T13:56:00Z", clock) == 60.0);
    // A time in the spring-forward gap takes the offset before the change (EST).
    CHECK(elapsed_seconds("2026-03-08T02:30:00", "2026-03-08T07:30:00Z", clock) == 0.0);
    // An ambiguous fall-back time takes the first (daylight) offset.
    CHECK(elapsed_seconds("2026-11-01T01:30:00", "2026-11-01T05:30:00Z", clock) == 0.0);
}

TEST_CASE("meeting identity lines use the clock's zone", "[test_meeting_time_utils]") {
    const fv::FakeClock clock(utc(2026, 8, 20), "America/New_York");
    const Json meeting = {{"started_at", "2026-08-20T16:55:00Z"},
                          {"ended_at", "2026-08-20T18:00:30Z"},
                          {"paused_total_s", "30"}};
    CHECK(format_meeting_started_at(meeting["started_at"], clock) ==
          "Aug 20, 2026 \xC2\xB7 12:55 PM");
    CHECK(format_meeting_duration(meeting, clock) == "1h 5m");
    CHECK(format_meeting_identity_meta(meeting, clock) ==
          "Aug 20, 2026 \xC2\xB7 12:55 PM \xC2\xB7 1h 5m");
    CHECK(format_meeting_started_at(nullptr, clock) == "Unknown date");
    CHECK(format_meeting_identity_meta(Json{{"started_at", "later"}}, clock) == "Unknown date");
}
