// tests/test_meeting_runtime.py: the startup recovery scan and the recovery dialog's actions.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/utf8.h"
#include "meeting/runtime_messages.h"
#include "runtime_support.h"

using namespace fv::meeting;
using namespace fv::meeting::runtime_test;
using Catch::Matchers::ContainsSubstring;
using fv::Json;

namespace {

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return text;
}

} // namespace

// from tests/test_meeting_runtime.py::test_recovery_scan_emits_dead_sessions
TEST_CASE("test_recovery_scan_emits_dead_sessions", "[test_meeting_runtime]") {
    const std::vector<Json> meetings{Json{{"id", "m_dead"}, {"status", "active"}}};
    RuntimeRig rig([&meetings](MeetingRuntimeDeps& deps) {
        deps.find_recoverable = [&meetings](fv::meeting_state::MeetingRepository&) {
            return meetings;
        };
    });
    rig.probe().recovery_scan_worker();
    rig.drain();
    CHECK(rig.got.found == std::vector<std::vector<Json>>{meetings});
}

// from tests/test_meeting_runtime.py::test_recovery_scan_silent_when_none
TEST_CASE("test_recovery_scan_silent_when_none", "[test_meeting_runtime]") {
    RuntimeRig rig([](MeetingRuntimeDeps& deps) {
        deps.find_recoverable = [](fv::meeting_state::MeetingRepository&) {
            return std::vector<Json>{};
        };
    });
    rig.probe().recovery_scan_worker();
    rig.drain();
    CHECK(rig.got.found.empty());
    CHECK(rig.got.scan_failures.empty());
}

// from tests/test_meeting_runtime.py::test_recovery_scan_failure_is_visible_and_retryable
TEST_CASE("test_recovery_scan_failure_is_visible_and_retryable", "[test_meeting_runtime]") {
    bool fail = true;
    RuntimeRig rig([&fail](MeetingRuntimeDeps& deps) {
        deps.find_recoverable = [&fail](fv::meeting_state::MeetingRepository&) {
            if (fail) {
                throw std::runtime_error("database unavailable");
            }
            return std::vector<Json>{Json{{"id", "m_saved"}}};
        };
    });
    rig.probe().recovery_scan_worker();
    rig.drain();
    CHECK(rig.got.found.empty());
    REQUIRE_FALSE(rig.got.scan_failures.empty());
    CHECK_THAT(rig.got.scan_failures.back(), ContainsSubstring("database unavailable"));
    CHECK_THAT(rig.got.scan_failures.back(), ContainsSubstring("Retry"));
    CHECK(rig.got.scan_failures.back() ==
          "Recovery could not check saved meetings. Retry the scan; your meeting files have not "
          "been removed. Details: database unavailable");

    fail = false;
    rig.rt->retry_recovery_scan();
    rig.drain();
    CHECK(rig.got.found == std::vector<std::vector<Json>>{{Json{{"id", "m_saved"}}}});
    CHECK(std::ranges::count(rig.spawned_names(), std::string("meeting-recovery-retry")) == 1);
}

// from tests/test_meeting_runtime.py::test_finalize_recovered_passes_meeting_dict
TEST_CASE("test_finalize_recovered_passes_meeting_dict", "[test_meeting_runtime]") {
    struct Call {
        const fv::meeting_state::MeetingRepository* repository;
        std::string meeting_id;
        std::string language;
        const ModelLease* lease;
    };
    std::vector<Call> called;
    RuntimeRig rig([&called](MeetingRuntimeDeps& deps) {
        deps.finalize_recovered = [&called](fv::meeting_state::MeetingRepository& repository,
                                            const Json& meeting, const std::string& language,
                                            ModelLease& lease) {
            called.push_back({&repository, meeting.at("id").get<std::string>(), language, &lease});
            return true;
        };
    });
    rig.make_meeting("m_dead");
    rig.settings.update(Json{{"meeting_language", "en"}});

    rig.probe().finalize_recovered_worker("m_dead");
    rig.drain();

    REQUIRE(called.size() == 1);
    CHECK(called[0].meeting_id == "m_dead");
    CHECK(called[0].repository == rig.repo.get());
    CHECK(called[0].language == "en");
    // Recovery loads a speech model of its own, so it is handed the lease that frees the
    // dictation engine first.
    CHECK(called[0].lease == rig.lease.get());
    CHECK_THAT(lower(rig.got.statuses.back()), ContainsSubstring("finalized"));
    CHECK(rig.got.errors.empty());
}

// from tests/test_meeting_runtime.py::test_finalize_recovered_hands_off_to_the_retry_pipeline
TEST_CASE("test_finalize_recovered_hands_off_to_the_retry_pipeline", "[test_meeting_runtime]") {
    RuntimeRig rig([](MeetingRuntimeDeps& deps) {
        deps.finalize_recovered = [](fv::meeting_state::MeetingRepository&, const Json&,
                                     const std::string&, ModelLease&) {
            return true;
        };
    });
    rig.make_meeting("m_dead", Json{{"spool_dir", "/spool/m_dead"}, {"cloud_enabled", true}});
    rig.settings.update(Json{{"meeting_end_redecode", true}});
    std::vector<std::pair<std::string, std::string>> retried;
    rig.rerun = [&retried](const std::string& meeting_id, const RerunRequest& request) {
        retried.emplace_back(request.from_step, meeting_id);
        return Json{{"ok", true}, {"finalization", Json{{"status", "completed"}}}};
    };

    rig.probe().finalize_recovered_worker("m_dead");
    rig.drain();

    CHECK(retried == std::vector<std::pair<std::string, std::string>>{{"redecode", "m_dead"}});
}

// from tests/test_meeting_runtime.py::test_finalize_recovered_keeps_incomplete_capture_warning
TEST_CASE("test_finalize_recovered_keeps_incomplete_capture_warning", "[test_meeting_runtime]") {
    RuntimeRig rig([](MeetingRuntimeDeps& deps) {
        deps.finalize_recovered = [](fv::meeting_state::MeetingRepository& repository,
                                     const Json& meeting, const std::string&, ModelLease&) {
            repository.update_meeting(meeting.at("id").get<std::string>(),
                                      Json{{"status", "failed"}});
            return true;
        };
    });
    rig.make_meeting("m_gap", Json{{"spool_dir", "/saved"}}, "needs_recovery");
    rig.settings.update(Json{{"meeting_end_redecode", true}});

    rig.probe().finalize_recovered_worker("m_gap");
    rig.drain();

    CHECK_THAT(rig.got.statuses.back(), ContainsSubstring("audio remains incomplete"));
    CHECK(std::ranges::count(rig.spawned_names(), std::string("meeting-retry-finalization")) == 0);
}

// from tests/test_meeting_runtime.py::test_recovered_follow_up_matches_live_end_settings
TEST_CASE("test_recovered_follow_up_matches_live_end_settings", "[test_meeting_runtime]") {
    struct Case {
        Json meeting;
        bool redecode, polish, report;
        std::optional<std::string> expected;
    };
    const auto c = GENERATE(
        Case{Json{{"spool_dir", "/s"}, {"cloud_enabled", false}}, true, true, true, "redecode"},
        Case{Json{{"spool_dir", ""}, {"cloud_enabled", false}}, true, true, true, std::nullopt},
        Case{Json{{"spool_dir", "/s"}, {"cloud_enabled", true}}, false, true, true, "polish"},
        Case{Json{{"spool_dir", "/s"}, {"cloud_enabled", true}}, false, false, true,
             "consolidation"},
        Case{Json{{"spool_dir", "/s"}, {"cloud_enabled", true}}, false, false, false,
             std::nullopt});
    const Json settings{{"meeting_end_redecode", c.redecode},
                        {"meeting_end_polish", c.polish},
                        {"meeting_end_report", c.report}};
    CHECK(recovered_follow_up_step(c.meeting, settings) == c.expected);
}

// from tests/test_meeting_runtime.py::test_finalize_recovered_reports_failure
TEST_CASE("test_finalize_recovered_reports_failure", "[test_meeting_runtime]") {
    RuntimeRig rig([](MeetingRuntimeDeps& deps) {
        deps.finalize_recovered = [](fv::meeting_state::MeetingRepository&, const Json&,
                                     const std::string&, ModelLease&) {
            return false;
        };
    });
    rig.make_meeting("m_dead");
    rig.probe().finalize_recovered_worker("m_dead");
    rig.drain();
    REQUIRE_FALSE(rig.got.errors.empty());
    CHECK_THAT(lower(rig.got.errors.back()), ContainsSubstring("could not finalize"));
}

// from tests/test_meeting_runtime.py::test_discard_recovered_deletes_meeting_data
TEST_CASE("test_discard_recovered_deletes_meeting_data", "[test_meeting_runtime]") {
    RuntimeRig rig;
    const auto spool = rig.root.path() / "meetings" / "m_dead";
    std::filesystem::create_directories(spool);
    rig.make_meeting("m_dead", Json{{"spool_dir", fv::path_to_utf8(spool)}}, "active");
    rig.probe().discard_recovered_worker("m_dead");
    rig.drain();
    CHECK_FALSE(rig.repo->get_meeting("m_dead").has_value());
    CHECK_FALSE(std::filesystem::exists(spool));
    CHECK_THAT(lower(rig.got.statuses.back()), ContainsSubstring("discarded"));
}

TEST_CASE("a recovery Finalize of an unknown meeting is an error, not a crash",
          "[meeting][runtime]") {
    bool reached = false;
    RuntimeRig rig([&reached](MeetingRuntimeDeps& deps) {
        deps.finalize_recovered = [&reached](fv::meeting_state::MeetingRepository&, const Json&,
                                             const std::string&, ModelLease&) {
            reached = true; // an unknown meeting must not be finalized
            return false;
        };
    });
    rig.rt->finalize_recovered("m_missing");
    rig.drain();
    CHECK(rig.got.errors ==
          std::vector<std::string>{"Could not finalize the meeting: Unknown meeting 'm_missing'"});
    CHECK_FALSE(reached);
}
