// When automatic checks are due and when the notice shows: OpenWhisper's tests/test_app_update.py
// TestSecondsUntilAutoCheck and TestShowsUpdateNotice.

#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "update/schedule.h"
#include "update_support.h"

namespace update = fv::update;
using fv::Json;
using update::InstallChannel;
using update::UpdateStatus;
using namespace std::chrono_literals;
using namespace fv::update::test;

namespace {

std::chrono::system_clock::time_point now() {
    return at("2026-08-21T12:00:00+00:00");
}

std::optional<double> wait(const Json& settings, InstallChannel channel = InstallChannel::Installer,
                           bool source_configured = true) {
    return update::seconds_until_auto_check(settings, now(), std::chrono::hours{4}, channel,
                                            source_configured);
}

std::string ago(std::chrono::system_clock::duration delta) {
    return iso(now() - delta);
}

} // namespace

// from tests/test_app_update.py::TestSecondsUntilAutoCheck::test_disabled_never_checks
TEST_CASE("disabled never checks", "[test_app_update]") {
    CHECK_FALSE(wait(Json{{"update_check_enabled", false}}).has_value());
}

// from tests/test_app_update.py::TestSecondsUntilAutoCheck::test_git_checkout_never_checks (no
// FioriVoce build is a git checkout; a build without an update source is what never checks)
TEST_CASE("a build without an update source never checks", "[test_app_update]") {
    CHECK_FALSE(wait(Json::object(), InstallChannel::Installer, false).has_value());
    CHECK_FALSE(wait(Json::object(), InstallChannel::Source, false).has_value());
}

// from tests/test_app_update.py::TestSecondsUntilAutoCheck::test_source_copy_checks
TEST_CASE("source copy checks", "[test_app_update]") {
    CHECK(wait(Json::object(), InstallChannel::Source) == 0.0);
}

// from tests/test_app_update.py::TestSecondsUntilAutoCheck::test_missing_timestamp_is_due
TEST_CASE("missing timestamp is due", "[test_app_update]") {
    CHECK(wait(Json{{"update_check_enabled", true}}) == 0.0);
}

// from
// tests/test_app_update.py::TestSecondsUntilAutoCheck::test_waits_out_the_interval_from_the_last_success
TEST_CASE("waits out the interval from the last success", "[test_app_update]") {
    CHECK(wait(Json{{"update_last_check_at", ago(1h)}}) == 3.0 * 3600);
}

// from tests/test_app_update.py::TestSecondsUntilAutoCheck::test_stale_check_is_due
TEST_CASE("stale check is due", "[test_app_update]") {
    CHECK(wait(Json{{"update_last_check_at", ago(5h)}}) == 0.0);
}

// from
// tests/test_app_update.py::TestSecondsUntilAutoCheck::test_recent_failure_waits_an_hour_even_when_success_is_stale
TEST_CASE("recent failure waits an hour even when success is stale", "[test_app_update]") {
    const Json settings{{"update_last_check_at", ago(5h)},
                        {"update_last_failed_check_at", ago(20min)}};
    CHECK(wait(settings) == 40.0 * 60);
}

// from
// tests/test_app_update.py::TestSecondsUntilAutoCheck::test_launch_does_not_wait_out_a_recent_success
TEST_CASE("launch does not wait out a recent success", "[test_app_update]") {
    const Json settings{{"update_last_check_at", ago(5min)}};
    CHECK(update::seconds_until_auto_check(settings, now(), InstallChannel::Installer, true,
                                           true) == 0.0);
    CHECK(update::seconds_until_auto_check(settings, now(), InstallChannel::Installer, false,
                                           true) == 4 * 3600.0 - 5 * 60);
}

// from
// tests/test_app_update.py::TestSecondsUntilAutoCheck::test_launch_still_waits_an_hour_after_a_failure
TEST_CASE("launch still waits an hour after a failure", "[test_app_update]") {
    const Json settings{{"update_last_failed_check_at", ago(20min)}};
    CHECK(update::seconds_until_auto_check(settings, now(), InstallChannel::Installer, true,
                                           true) == 40.0 * 60);
}

// from
// tests/test_app_update.py::TestSecondsUntilAutoCheck::test_future_times_from_a_moved_clock_are_ignored
TEST_CASE("future times from a moved clock are ignored", "[test_app_update]") {
    CHECK(wait(Json{{"update_last_check_at", iso(now() + 48h)}}) == 0.0);
}

// from
// tests/test_app_update.py::TestSecondsUntilAutoCheck::test_invalid_timestamp_is_treated_as_missing
TEST_CASE("invalid timestamp is treated as missing", "[test_app_update]") {
    CHECK(wait(Json{{"update_last_check_at", "not-a-date"}}) == 0.0);
    CHECK_FALSE(update::parse_last_check_at(Json("not-a-date")).has_value());
}

// from tests/test_app_update.py::TestShowsUpdateNotice::test_update_available_shows_by_default
TEST_CASE("update available shows by default", "[test_app_update]") {
    CHECK(update::shows_update_notice(UpdateStatus::UpdateAvailable, Json::object()));
}

// from
// tests/test_app_update.py::TestShowsUpdateNotice::test_nothing_new_or_an_opt_out_shows_nothing
TEST_CASE("nothing new or an opt out shows nothing", "[test_app_update]") {
    CHECK_FALSE(update::shows_update_notice(UpdateStatus::UpToDate, Json::object()));
    CHECK_FALSE(update::shows_update_notice(UpdateStatus::Development, Json::object()));
    CHECK_FALSE(update::shows_update_notice(UpdateStatus::UpdateAvailable,
                                            Json{{"update_notify_enabled", false}}));
    CHECK_FALSE(update::shows_update_notice(UpdateStatus::UpdateAvailable,
                                            Json{{"update_check_enabled", false}}));
}

TEST_CASE("check times round-trip as datetime.isoformat() writes them", "[update]") {
    const auto time = at("2026-08-21T12:00:00.250000+00:00");
    CHECK(iso(time) == "2026-08-21T12:00:00.250000+00:00");
    CHECK(iso(now()) == "2026-08-21T12:00:00+00:00");
    // A finer clock (Windows ticks are 100 ns) is floored to the microsecond, as datetime.now().
    const auto finer = time + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                  std::chrono::nanoseconds{900});
    CHECK(iso(finer) == "2026-08-21T12:00:00.250000+00:00");
    const auto parsed = update::parse_last_check_at(Json("2026-08-21T13:30:00+01:30"));
    REQUIRE(parsed);
    CHECK(update::to_sys_time(*parsed) == at("2026-08-21T12:00:00+00:00"));
}
