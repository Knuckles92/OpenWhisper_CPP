// tests/test_meeting_web_history.py: bounded History reads keep paging, content capabilities and
// host access, over a SQL repository in a temporary data folder.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <memory>
#include <string>
#include <vector>

#include "api_harness.h"
#include "base/base64.h"
#include "meeting_web/testing/fake_dashboard_engine.h"
#include "pyport/json.h"

using fv::Json;
using namespace fv::meeting_web::test_support;
namespace mw = fv::meeting_web;
namespace ms = fv::meeting_state;

namespace {

const Query kHost{{"token", "host-token"}};

// tests/helpers.py make_meeting: a bare active meeting row.
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

void saved(ms::SqlMeetingRepository& repo, const std::string& meeting_id,
           const std::string& title = "Saved", const Json& state = Json::object()) {
    make_meeting(repo, meeting_id);
    repo.update_meeting(meeting_id, {{"status", "ended"},
                                     {"title", title},
                                     {"started_at", "2026-09-01T00:00:00Z"},
                                     {"state_json", state.dump()}});
}

ms::TranscriptSegment make_segment(const std::string& meeting_id, const std::string& segment_id,
                                   double start = 1.0, const std::string& text = "hello world") {
    ms::TranscriptSegment segment;
    segment.segment_id = segment_id;
    segment.meeting_id = meeting_id;
    segment.channel = "mic";
    segment.start_s = start;
    segment.end_s = start + 2.0;
    segment.text = text;
    return segment;
}

std::vector<std::string> ids(const Json& rows) {
    std::vector<std::string> out;
    for (const auto& row : rows) {
        out.push_back(row.at("id").get<std::string>());
    }
    return out;
}

std::string urlsafe_b64(std::string_view bytes) {
    return fv::b64encode(bytes, true, false);
}

// The history_client fixture: m_current is the dashboard's meeting.
struct HistoryClient {
    HistoryClient() {
        make_meeting(*world.repo, "m_current");
        world.repo->update_meeting("m_current", {{"started_at", "2026-01-01T00:00:00Z"}});
        engine = std::make_unique<mw::testing::FakeDashboardEngine>("m_current");
        app = std::make_unique<ApiServer>(
            *engine, world.repo, mw::sql_past_meeting_pages(world.repo), mw::PostMeetingPasses{},
            mw::DashboardOptions{}, world.root.path() / "settings.json", world.clock);
    }

    SqlWorld world;
    std::unique_ptr<mw::testing::FakeDashboardEngine> engine;
    std::unique_ptr<ApiServer> app;
};

// test_meeting_web_history.py _BAD_CURSORS.
std::vector<std::string> bad_cursors() {
    return {
        "bad", "!!!", "e30", "WzEsMiwzXQ", "MQ", "bnVsbA", urlsafe_b64(std::string(100000, '['))};
}

} // namespace

// from
// tests/test_meeting_web_history.py::test_history_uses_bounded_aggregate_reads_and_stable_keyset_ties
TEST_CASE("test_history_uses_bounded_aggregate_reads_and_stable_keyset_ties",
          "[test_meeting_web_history]") {
    HistoryClient client;
    auto& repo = *client.world.repo;
    for (int i = 0; i < 10; ++i) {
        const std::string id = "m_" + std::to_string(i);
        saved(repo, id);
        std::vector<ms::TranscriptSegment> segments;
        for (int j = 0; j < 30; ++j) {
            segments.push_back(
                make_segment(id, "s_" + std::to_string(i) + "_" + std::to_string(j), j, "Preview"));
        }
        repo.add_segments(segments);
    }
    const auto response =
        client.app->get("/api/meetings", {{"token", "host-token"}, {"limit", "3"}});
    REQUIRE(response.status == 200);
    const Json first = response.json();
    CHECK(ids(first["meetings"]) == std::vector<std::string>{"m_9", "m_8", "m_7"});
    // Five SELECTs: the token check's meeting row, then bounded sessions, audio, counts and the
    // first preview, the page's four aggregate reads.
    CHECK(repo.last_summary_query_count() == 4);
    CHECK(first["meetings"][0]["content_summary"]["transcript_segments"] == 30);
    for (const auto& row : first["meetings"]) {
        CHECK_FALSE(row.contains("state_json"));
        CHECK_FALSE(row.contains("host_token"));
        CHECK_FALSE(row.contains("guest_token"));
    }
    std::vector<std::string> all = ids(first["meetings"]);
    Json cursor = first["next_cursor"];
    while (!cursor.is_null()) {
        const Json page = client.app
                              ->get("/api/meetings", {{"token", "host-token"},
                                                      {"limit", "3"},
                                                      {"cursor", cursor.get<std::string>()}})
                              .json();
        for (const auto& id : ids(page["meetings"])) {
            all.push_back(id);
        }
        cursor = page["next_cursor"];
    }
    std::vector<std::string> expected;
    for (int i = 9; i >= 0; --i) {
        expected.push_back("m_" + std::to_string(i));
    }
    expected.push_back("m_current");
    CHECK(all == expected);
}

// from
// tests/test_meeting_web_history.py::test_filtered_empty_window_keeps_a_continuation_and_query_is_literal
TEST_CASE("test_filtered_empty_window_keeps_a_continuation_and_query_is_literal",
          "[test_meeting_web_history]") {
    HistoryClient client;
    auto& repo = *client.world.repo;
    saved(repo, "m_3");
    saved(repo, "m_2", "Literal 100%_exact",
          Json::parse(R"({"cards": {"decisions": [{"id": "d", "status": "confirmed"}]}})"));
    saved(repo, "m_1", "Literal 100ZZexact",
          Json::parse(R"({"cards": {"decisions": [{"id": "d", "status": "removed"}]}})"));
    const Json first =
        client.app
            ->get("/api/meetings",
                  {{"token", "host-token"}, {"limit", "1"}, {"has_decisions", "true"}})
            .json();
    CHECK(first["meetings"] == Json::array());
    REQUIRE(first["next_cursor"].is_string());
    const Json second =
        client.app
            ->get("/api/meetings", {{"token", "host-token"},
                                    {"limit", "1"},
                                    {"has_decisions", "true"},
                                    {"cursor", first["next_cursor"].get<std::string>()}})
            .json();
    CHECK(ids(second["meetings"]) == std::vector<std::string>{"m_2"});
    const Json literal =
        client.app->get("/api/meetings", {{"token", "host-token"}, {"q", "100%_"}}).json();
    CHECK(ids(literal["meetings"]) == std::vector<std::string>{"m_2"});
    const Json recent =
        client.app
            ->get("/api/meetings", {{"token", "host-token"}, {"since", "2026-10-01T00:00:00Z"}})
            .json();
    CHECK(recent["meetings"] == Json::array());
}

// from tests/test_meeting_web_history.py::test_preview_and_poll_do_not_query_transcript_pages
TEST_CASE("test_preview_and_poll_do_not_query_transcript_pages", "[test_meeting_web_history]") {
    HistoryClient client;
    auto& repo = *client.world.repo;
    saved(repo, "m_saved");
    repo.add_segments({make_segment("m_saved", "s1")});
    // The preview reads the content summary from its aggregate reads, never a transcript page.
    const auto preview = client.app->get(
        "/api/meetings/m_saved", {{"token", "host-token"}, {"include_transcript", "False"}});
    REQUIRE(preview.status == 200);
    CHECK(preview.json()["segments"] == Json::array());
    CHECK(preview.json()["transcript_included"] == false);
    CHECK(preview.json()["transcript_next_cursor"].is_null());
    CHECK(preview.json()["meeting"]["has_transcript"] == true);
    const auto state = client.app->get("/api/meetings/m_saved/state", kHost);
    REQUIRE(state.status == 200);
    CHECK(state.json()["state"]["meeting_id"] == "m_saved");
    // The poll carries the state alone.
    CHECK(state.json().size() == 1);
}

// from
// tests/test_meeting_web_history.py::test_history_paging_and_state_preserve_auth_and_bad_cursor_errors
TEST_CASE("test_history_paging_and_state_preserve_auth_and_bad_cursor_errors",
          "[test_meeting_web_history]") {
    HistoryClient client;
    for (const char* route : {"/api/meetings", "/api/meetings/m_current/state"}) {
        CHECK(client.app->get(route, {{"token", "guest-token"}}).status == 403);
        CHECK(client.app->get(route, {{"token", "wrong"}}).status == 401);
    }
    CHECK(client.app->get("/api/meetings", {{"token", "host-token"}, {"cursor", "bad"}}).status ==
          400);
    CHECK(client.app->get("/api/meetings/m_missing/state", kHost).status == 404);
}

// from tests/test_meeting_web_history.py::test_malformed_history_cursors_are_client_errors
TEST_CASE("test_malformed_history_cursors_are_client_errors", "[test_meeting_web_history]") {
    HistoryClient client;
    auto cursors = bad_cursors();
    cursors.push_back("WzEsMl0");              // [1,2]: fields are not strings
    cursors.push_back(std::string(5000, 'x')); // too long
    for (const auto& cursor : cursors) {
        INFO(cursor.substr(0, 40));
        // Through the handler: a 136 KB cursor is past any server's request-line limit.
        const auto r = client.app->api->call("GET", "/api/meetings",
                                             {{"token", "host-token"}, {"cursor", cursor}});
        CHECK(r.status == 400);
        CHECK(r.body == R"({"detail":"invalid meeting cursor"})");
    }
}

// from tests/test_meeting_web_history.py::test_malformed_transcript_cursors_are_client_errors
TEST_CASE("test_malformed_transcript_cursors_are_client_errors", "[test_meeting_web_history]") {
    HistoryClient client;
    auto cursors = bad_cursors();
    cursors.push_back("WyJhIiwiYiJd"); // ["a","b"]: start is not a number
    cursors.push_back(urlsafe_b64("[" + std::string(400, '9') + ",\"s\"]")); // overflows float
    for (const auto& cursor : cursors) {
        INFO(cursor.substr(0, 40));
        const auto r = client.app->api->call("GET", "/api/transcript",
                                             {{"token", "host-token"}, {"cursor", cursor}});
        CHECK(r.status == 400);
        CHECK(r.body == R"({"detail":"invalid transcript cursor"})");
    }
}
