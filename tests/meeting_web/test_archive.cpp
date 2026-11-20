// meeting_web/archive.h: the four adapter tests of tests/test_meeting_archive_dashboard.py (the
// runtime's history URL tests are the meeting runtime's).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

#include "api_harness.h"
#include "base/utf8.h"
#include "fake_web_repo.h"
#include "meeting_state/schema.h"
#include "meeting_web/archive.h"
#include "meeting_web/server.h"
#include "net/reactor.h"
#include "pyport/errors.h"
#include "web_test_support.h"

using fv::Json;
using namespace fv::meeting_web::test_support;
namespace mw = fv::meeting_web;
namespace ms = fv::meeting_state;

namespace {

Json archive_meeting() {
    ms::MeetingState state;
    state.meeting_id = "m_archive";
    state.title = "Old snapshot title";
    state.status = "active";
    return Json{{"id", "m_archive"},
                {"title", "Quarterly review"},
                {"status", "ended"},
                {"state_json", state.to_json().dump()},
                {"state_seq", 4},
                {"cloud_enabled", false},
                {"agent_provider", "openrouter"},
                {"agent_model", "test-model"}};
}

std::shared_ptr<FakeWebRepo> archive_repo() {
    auto repo = std::make_shared<FakeWebRepo>();
    repo->set_segments({Json{{"id", "sg_1"}, {"meeting_id", "m_archive"}, {"start_s", 1.0}}});
    return repo;
}

} // namespace

// from
// tests/test_meeting_archive_dashboard.py::test_archive_adapter_normalizes_persisted_lifecycle_and_transcript
TEST_CASE("test_archive_adapter_normalizes_persisted_lifecycle_and_transcript",
          "[test_meeting_archive_dashboard]") {
    const auto repo = archive_repo();
    mw::ArchivedMeetingDashboard archive(repo, archive_meeting(), "meetings");
    const Json snapshot = archive.store()->snapshot();
    CHECK(snapshot["meeting_id"] == "m_archive");
    CHECK(snapshot["title"] == "Quarterly review");
    CHECK(snapshot["status"] == "ended");
    CHECK_FALSE(archive.is_active());
    CHECK(archive.get_transcript() ==
          std::vector<Json>{Json{{"id", "sg_1"}, {"meeting_id", "m_archive"}, {"start_s", 1.0}}});
    CHECK(archive.meeting_id() == "m_archive");
    CHECK(archive.spool_root() == "meetings");
}

// from
// tests/test_meeting_archive_dashboard.py::test_archive_adapter_normalizes_interrupted_finalization
TEST_CASE("test_archive_adapter_normalizes_interrupted_finalization",
          "[test_meeting_archive_dashboard]") {
    // Archived snapshots must not claim cloud consolidation is still running.
    ms::MeetingState state;
    state.meeting_id = "m_archive";
    state.title = "Interrupted finalize";
    state.status = "ended";
    state.cloud_enabled = true;
    Json payload = state.to_json();
    payload["finalization"] = {{"status", "running"},
                               {"message", "Preparing final cloud insights…"}};
    const Json meeting{{"id", "m_archive"},
                       {"title", "Interrupted finalize"},
                       {"status", "ended"},
                       {"state_json", payload.dump()},
                       {"state_seq", 7},
                       {"cloud_enabled", true},
                       {"agent_provider", "openrouter"},
                       {"agent_model", "test-model"}};
    mw::ArchivedMeetingDashboard archive(archive_repo(), meeting, "meetings");
    const Json snapshot = archive.store()->snapshot();
    CHECK(snapshot["finalization"]["status"] == "failed");
    std::string message = snapshot["finalization"]["message"].get<std::string>();
    std::ranges::transform(message, message.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    CHECK(message.find("interrupted") != std::string::npos);
}

// from tests/test_meeting_archive_dashboard.py::test_archive_adapter_rotates_links_and_stops_server
TEST_CASE("test_archive_adapter_rotates_links_and_stops_server",
          "[test_meeting_archive_dashboard]") {
    const auto repo = archive_repo();
    Json row = archive_meeting();
    row["host_token"] = "archive-host";
    row["guest_token"] = "archive-guest";
    repo->set_meeting(row);
    mw::ArchivedMeetingDashboard archive(repo, row, "meetings");
    fv::net::Reactor reactor;
    mw::MeetingWebServer server(reactor, archive, repo);
    REQUIRE(server.start().has_value());
    archive.attach_server(&server);
    CHECK(archive.is_running());
    CHECK(archive.host_url() == server.base_url() + "/m/archive-host");
    const auto port = static_cast<std::uint16_t>(
        std::stoi(server.base_url().substr(server.base_url().rfind(':') + 1)));
    Client host(port, "token=archive-host");
    CHECK(host.receive()["type"] == "hello");

    const Json urls = archive.regenerate_tokens();

    const auto tokens = repo->tokens();
    REQUIRE(tokens.size() == 3);
    CHECK(tokens[0] == "m_archive");
    CHECK(tokens[1].size() == 43);
    CHECK(tokens[2].size() == 43);
    CHECK(tokens[1] != tokens[2]);
    // The sockets of the old pair are closed (the server was invalidated).
    CHECK(host.closed_with() == 4401);
    CHECK(urls == Json{{"host_url", server.host_url()}, {"guest_url", server.guest_url()}});

    archive.shutdown();
    CHECK_FALSE(server.is_running());
    CHECK_FALSE(archive.is_running());
    CHECK(archive.host_url().empty());
}

// from
// tests/test_meeting_archive_dashboard.py::test_archive_adapter_serves_an_authenticated_session
TEST_CASE("test_archive_adapter_serves_an_authenticated_session",
          "[test_meeting_archive_dashboard]") {
    SqlWorld world;
    const Json meeting = archive_meeting();
    world.repo->create_meeting({{"id", meeting["id"]},
                                {"title", meeting["title"]},
                                {"status", meeting["status"]},
                                {"started_at", "2025-01-02T09:30:00"},
                                {"ended_at", "2025-01-02T10:00:00"},
                                {"host_token", "archive-host"},
                                {"guest_token", "archive-guest"},
                                {"cloud_enabled", false},
                                {"spool_dir", fv::path_to_utf8(world.root.path() / "spool")},
                                {"state_json", meeting["state_json"]},
                                {"state_seq", meeting["state_seq"]}});
    mw::ArchivedMeetingDashboard archive(world.repo, *world.repo->get_meeting("m_archive"),
                                         world.root.path());
    {
        ApiServer app(archive, world.repo, mw::sql_past_meeting_pages(world.repo),
                      mw::PostMeetingPasses{}, mw::DashboardOptions{},
                      world.root.path() / "settings.json", world.clock);
        archive.attach_server(app.server.get());
        const auto response = app.get("/api/session", {{"token", "archive-host"}});
        REQUIRE(response.status == 200);
        const Json payload = response.json();
        CHECK(payload["role"] == "host");
        CHECK(payload["state"]["meeting_id"] == "m_archive");
        CHECK(payload["state"]["status"] == "ended");

        // The live controls do nothing; the AI insights switch is refused with a clean 409.
        CHECK(app.post("/api/meeting/end", {{"token", "archive-host"}}).status == 200);
        const auto cloud = app.post_json("/api/meeting/cloud", {{"token", "archive-host"}},
                                         Json{{"enabled", false}});
        CHECK(cloud.status == 409);
        CHECK(cloud.json()["detail"] == "AI insights are unavailable for archived playback");
        archive.shutdown();
        CHECK_FALSE(app.server->is_running());
    }
}

TEST_CASE("an archive guest is a participant of the stored meeting", "[archive]") {
    SqlWorld world;
    world.repo->create_meeting({{"id", "m_archive"},
                                {"title", "Quarterly review"},
                                {"status", "ended"},
                                {"started_at", "2025-01-02T09:30:00"},
                                {"host_token", "archive-host"},
                                {"guest_token", "archive-guest"},
                                {"spool_dir", ""},
                                {"state_json", archive_meeting()["state_json"]}});
    mw::ArchivedMeetingDashboard archive(world.repo, *world.repo->get_meeting("m_archive"), "");
    const Json guest = archive.add_guest("   " + std::string(100, 'n') + "  ");
    CHECK(guest["display_name"] == std::string(80, 'n'));
    CHECK(guest["kind"] == "guest");
    CHECK(archive.add_guest("   ")["display_name"] == "Guest");
    // The edit is persisted: the stored row's snapshot has both guests.
    const Json stored =
        Json::parse(world.repo->get_meeting("m_archive")->at("state_json").get<std::string>());
    CHECK(stored["participants"].size() == 2);
    CHECK_THROWS_AS(archive.set_cloud_enabled(true), mw::EngineRefusal);
    CHECK_THROWS_AS(mw::ArchivedMeetingDashboard(world.repo, Json{{"title", "no id"}}, ""),
                    fv::py::PyError);
}
