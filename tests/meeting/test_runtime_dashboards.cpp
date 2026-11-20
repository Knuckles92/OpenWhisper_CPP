// tests/test_meeting_archive_dashboard.py, the four runtime tests: opening a past meeting reuses
// the dashboard and targets the selected meeting (and its report view); plus the app's own
// archive dashboard on a loopback server.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "meeting/runtime_messages.h"
#include "net/reactor.h"
#include "runtime_support.h"

using namespace fv::meeting;
using namespace fv::meeting::runtime_test;
using fv::Json;

// from
// tests/test_meeting_archive_dashboard.py::test_runtime_reuses_dashboard_and_opens_selected_history
TEST_CASE("test_runtime_reuses_dashboard_and_opens_selected_history",
          "[test_meeting_archive_dashboard]") {
    RuntimeRig rig;
    rig.make_meeting("m_selected");
    rig.probe().set_host_url("http://127.0.0.1:9000/m/host-token");
    rig.probe().open_past_meeting_worker("m_selected");
    rig.drain();
    CHECK(rig.opened_urls() ==
          std::vector<std::string>{"http://127.0.0.1:9000/m/host-token?history=m_selected"});
    CHECK(rig.got.errors.empty());
    CHECK(rig.got.statuses == std::vector<std::string>{"Opening past meeting"});
    CHECK(rig.archives.empty());
}

// from
// tests/test_meeting_archive_dashboard.py::test_history_target_preserves_existing_query_and_capability_path
TEST_CASE("test_history_target_preserves_existing_query_and_capability_path",
          "[test_meeting_archive_dashboard]") {
    CHECK(with_history_target("http://127.0.0.1:9000/m/secret-token?source=qt", "m_review/one") ==
          "http://127.0.0.1:9000/m/secret-token?source=qt&history=m_review%2Fone");
}

// from tests/test_meeting_archive_dashboard.py::test_history_target_can_focus_report_view
TEST_CASE("test_history_target_can_focus_report_view", "[test_meeting_archive_dashboard]") {
    CHECK(with_history_target("http://127.0.0.1:9000/m/secret-token?source=qt", "m_review/one",
                              "report") ==
          "http://127.0.0.1:9000/m/secret-token?source=qt&history=m_review%2Fone&view=report");
}

// from tests/test_meeting_archive_dashboard.py::test_runtime_opens_report_view_on_history_url
TEST_CASE("test_runtime_opens_report_view_on_history_url", "[test_meeting_archive_dashboard]") {
    RuntimeRig rig;
    rig.make_meeting("m_selected");
    rig.probe().set_host_url("http://127.0.0.1:9000/m/host-token");
    rig.probe().open_past_meeting_worker("m_selected", "report");
    rig.drain();
    CHECK(rig.opened_urls() ==
          std::vector<std::string>{
              "http://127.0.0.1:9000/m/host-token?history=m_selected&view=report"});
    CHECK(rig.got.statuses == std::vector<std::string>{"Opening meeting report"});
}

TEST_CASE("a past meeting opens in the app's archive dashboard on loopback", "[meeting][runtime]") {
    fv::net::Reactor reactor;
    RuntimeRig rig([&reactor](MeetingRuntimeDeps& deps) {
        deps.reactor = &reactor;
        deps.archive_factory = {}; // the app's own
    });
    rig.make_ended_meeting("m_past", false, "completed");

    rig.rt->open_past_meeting("m_past");
    rig.drain();

    REQUIRE(rig.opened_urls().size() == 1);
    const std::string url = rig.opened_urls()[0];
    CHECK(url.starts_with("http://127.0.0.1:"));
    CHECK(url.ends_with("/m/host-m_past?history=m_past"));
    const auto archive = rig.probe().archive();
    REQUIRE(archive);
    CHECK(archive->is_running());
    CHECK(rig.rt->meeting_in_use("m_other"));

    // The archive's dashboard owns the meeting's title while it runs.
    rig.rt->retitle_saved_meeting("m_past", "Quarterly review");
    CHECK(rig.repo->get_meeting("m_past")->at("title") == "Quarterly review");

    // A second open reuses the running archive.
    rig.rt->open_past_meeting("m_past", true);
    rig.drain();
    REQUIRE(rig.opened_urls().size() == 2);
    CHECK(rig.opened_urls()[1] == url + "&view=report");
    CHECK(rig.probe().archive() == archive);

    rig.rt->cleanup();
    CHECK_FALSE(archive->is_running());
}
