// tests/test_meeting_title_consistency.py, the three dashboard tests: archive dashboards over one
// disposable database, each on its own loopback server, refreshed after another owner renames.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <chrono>
#include <memory>
#include <string>

#include "api_harness.h"
#include "meeting_state/stored.h"
#include "meeting_web/archive.h"
#include "pyport/json.h"
#include "web_test_support.h"

using fv::Json;
using namespace fv::meeting_web::test_support;
namespace mw = fv::meeting_web;
namespace ms = fv::meeting_state;
using namespace std::chrono_literals;

namespace {

const Query kHost{{"token", "host-token"}};

void make_meeting(ms::SqlMeetingRepository& repo, const std::string& meeting_id) {
    repo.create_meeting({{"id", meeting_id},
                         {"title", "Test meeting"},
                         {"status", "active"},
                         {"started_at", "2026-11-17T09:00:00"},
                         {"host_token", "host-token"},
                         {"guest_token", "guest-token"},
                         {"cloud_enabled", false},
                         {"spool_dir", "/tmp/spool"}});
}

// The title_case fixture: an ended meeting with four finished reports, and another meeting.
struct TitleCase {
    TitleCase() {
        make_meeting(*world.repo, meeting_id);
        ms::MeetingState state;
        state.meeting_id = meeting_id;
        state.title = "Test meeting";
        state.status = "ended";
        Json document = state.to_json();
        document["custom_reports"] = Json::array();
        for (int i = 0; i < 4; ++i) {
            document["custom_reports"].push_back(Json{
                {"id", "r_" + std::to_string(i)}, {"request", "Summary"}, {"status", "completed"}});
        }
        world.repo->persist_state(meeting_id, document);
        make_meeting(*world.repo, other_id);
    }

    SqlWorld world;
    std::string meeting_id = "m_test1";
    std::string other_id = "m_other";
};

// InProcessDashboard: an archive dashboard of one meeting on its own server.
struct Dashboard {
    Dashboard(TitleCase& c, const std::string& meeting_id)
        : engine(c.world.repo, *c.world.repo->get_meeting(meeting_id), "unused", Json::object(),
                 c.world.clock) {
        app = std::make_unique<ApiServer>(
            engine, c.world.repo, mw::sql_past_meeting_pages(c.world.repo), mw::PostMeetingPasses{},
            mw::DashboardOptions{}, c.world.root.path() / ("settings-" + meeting_id + ".json"),
            c.world.clock);
        engine.attach_server(app->server.get());
    }
    ~Dashboard() { engine.attach_server(nullptr); }

    Json delete_report(const std::string& meeting_id, const std::string& report_id) {
        const auto response =
            app->del("/api/meetings/" + meeting_id + "/reports/" + report_id, kHost);
        REQUIRE(response.status == 200);
        return response.json();
    }
    Json detail(const std::string& meeting_id) {
        const auto response = app->get("/api/meetings/" + meeting_id, kHost);
        REQUIRE(response.status == 200);
        return response.json();
    }

    mw::ArchivedMeetingDashboard engine;
    std::unique_ptr<ApiServer> app;
};

void assert_saved_title(TitleCase& c, const std::string& meeting_id, const std::string& title) {
    const Json row = *c.world.repo->get_meeting(meeting_id);
    CHECK(row["title"] == title);
    CHECK(fv::py::loads(row["state_json"].get<std::string>())["title"] == title);
    CHECK(ms::open_store(c.world.repo, meeting_id, row, true, Json::object(), c.world.clock)
              ->snapshot()["title"] == title);
}

} // namespace

// from
// tests/test_meeting_title_consistency.py::test_refresh_of_current_dashboard_reconnects_without_writing_state
TEST_CASE("test_refresh_of_current_dashboard_reconnects_without_writing_state",
          "[test_meeting_title_consistency]") {
    TitleCase c;
    Dashboard dashboard(c, c.meeting_id);
    // The invalidation shows as the open socket's close (4409), and only once.
    Client first(dashboard.app->port, "token=host-token");
    CHECK(first.receive()["type"] == "hello");
    Json original = dashboard.engine.store()->snapshot();
    c.world.repo->rename_meeting(c.meeting_id, "External rename");
    dashboard.app->api->refresh_saved_meeting_title(c.meeting_id);
    original["title"] = "External rename";
    CHECK(dashboard.engine.store()->snapshot() == original);
    CHECK(c.world.repo->list_events(c.meeting_id).empty());
    CHECK(first.closed_with() == 4409);

    Client second(dashboard.app->port, "token=host-token");
    CHECK(second.receive()["type"] == "hello");
    dashboard.app->api->refresh_saved_meeting_title(c.meeting_id);
    CHECK_FALSE(second.raw().wait_closed(500ms));
}

// from
// tests/test_meeting_title_consistency.py::test_cached_report_delete_after_mcp_rename_does_not_restore_old_title
TEST_CASE("test_cached_report_delete_after_mcp_rename_does_not_restore_old_title",
          "[test_meeting_title_consistency]") {
    TitleCase c;
    Dashboard primary(c, c.other_id);
    Dashboard secondary(c, c.other_id);
    secondary.delete_report(c.meeting_id, "r_0");
    // The runtime's retitle (an MCP rename): the first running dashboard writes it, every other
    // one refreshes its cache.
    CHECK(primary.app->api->retitle_saved_meeting(c.meeting_id, "Renamed through MCP"));
    secondary.app->api->refresh_saved_meeting_title(c.meeting_id);
    secondary.delete_report(c.meeting_id, "r_1");
    assert_saved_title(c, c.meeting_id, "Renamed through MCP");
    CHECK(secondary.detail(c.meeting_id)["state"]["title"] == "Renamed through MCP");
}

// from
// tests/test_meeting_title_consistency.py::test_refreshed_dashboard_socket_resyncs_without_expiring_the_host_token
TEST_CASE("test_refreshed_dashboard_socket_resyncs_without_expiring_the_host_token",
          "[test_meeting_title_consistency]") {
    TitleCase c;
    Dashboard dashboard(c, c.meeting_id);
    {
        Client socket(dashboard.app->port, "token=host-token");
        CHECK(socket.receive()["state"]["title"] == "Test meeting");
        c.world.repo->rename_meeting(c.meeting_id, "External rename");
        dashboard.app->api->refresh_saved_meeting_title(c.meeting_id);
        CHECK(socket.closed_with() == 4409);
    }
    Client socket(dashboard.app->port, "token=host-token");
    CHECK(socket.receive()["state"]["title"] == "External rename");
}

TEST_CASE("a local retitle needs a running dashboard and follows the rename contract", "[titles]") {
    TitleCase c;
    Dashboard dashboard(c, c.other_id);
    auto& api = *dashboard.app->api;
    CHECK_THROWS_WITH(api.retitle_saved_meeting(c.meeting_id, ""),
                      "Supply a title of 1–200 characters without control characters.");
    try {
        api.retitle_saved_meeting("m_missing", "Nobody");
        FAIL("an unknown meeting is refused");
    } catch (const mw::HttpError& error) {
        CHECK(error.status() == 404);
    }
    // The current meeting goes through the engine's own store.
    CHECK(api.retitle_saved_meeting(c.other_id, "  Fresh title "));
    CHECK(dashboard.engine.store()->snapshot()["title"] == "Fresh title");
    dashboard.app->server->stop();
    CHECK_THROWS_WITH(api.retitle_saved_meeting(c.meeting_id, "Later"),
                      "Meeting dashboard is unavailable.");
    api.refresh_saved_meeting_title(c.meeting_id); // nothing while stopped
}
