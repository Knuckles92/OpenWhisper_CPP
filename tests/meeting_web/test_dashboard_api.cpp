// meeting_web/dashboard_api.h over a loopback server: tests/test_meeting_web_auth.py (31 of 42),
// the seven REST tests of tests/test_meeting_custom_report.py, the web download test of
// tests/test_meeting_export_features.py, and the routes M7 answers without the meeting
// intelligence.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../meeting_state/export_fixture.h"
#include "api_harness.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "fake_web_repo.h"
#include "meeting_state/custom_report_ops.h"
#include "meeting_state/export_bulk.h"
#include "meeting_state/store.h"
#include "meeting_web/extensions.h"
#include "meeting_web/testing/fake_dashboard_engine.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "settings/keys.h"
#include "web_test_support.h"

using fv::Json;
using namespace fv::meeting_web::test_support;
namespace mw = fv::meeting_web;
namespace ms = fv::meeting_state;
using namespace std::chrono_literals;

namespace {

const std::string kHost(kFakeHostToken);
const std::string kGuest(kFakeGuestToken);
const Query kHostQuery{{"token", kHost}};
const Query kGuestQuery{{"token", kGuest}};

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

// A finished re-run: its state document is the meeting's own.
Json canned_rerun(const std::string& meeting_id) {
    return Json{{"ok", true},
                {"state", Json{{"meeting_id", meeting_id}, {"seq", 3}}},
                {"applied", 2},
                {"error", nullptr}};
}

// tests/test_meeting_web_auth.py's `client` fixture: FakeWebEngine and FakeWebRepo behind the
// app. The engine's store is FakeWebStore's document.
struct WebClient {
    explicit WebClient(mw::PostMeetingPasses passes = default_passes(),
                       std::optional<std::filesystem::path> dist = std::nullopt,
                       ms::MeetingState state = fake_web_state(),
                       decltype(mw::DashboardOptions::request_cloud_consent) consent = {}) {
        store = std::make_shared<ms::MeetingStateStore>(std::move(state), nullptr,
                                                        ms::SegmentHandler{}, clock, ids);
        engine = std::make_unique<mw::testing::FakeDashboardEngine>("m_test", store,
                                                                    root.path() / "meetings");
        engine->set_regenerate(
            [] { return Json{{"host_url", "/m/new-host"}, {"guest_url", "/m/new-guest"}}; });
        mw::DashboardOptions options;
        options.dist_root = dist.value_or(root.path() / "no-bundle");
        options.meetings_root = root.path() / "meetings";
        options.request_cloud_consent = std::move(consent);
        app = std::make_unique<ApiServer>(*engine, repo, repo, std::move(passes),
                                          std::move(options), root.path() / "settings.json", clock);
    }

    static mw::PostMeetingPasses default_passes() {
        mw::PostMeetingPasses passes;
        passes.rerun_insights = [](const std::string& meeting_id, const std::string&,
                                   const fv::CancelToken&) {
            return canned_rerun(meeting_id);
        };
        passes.rerun_speakers = [](const std::string&, std::shared_ptr<ms::MeetingStateStore>) {
            return Json{{"ok", true}, {"relabeled", 3}};
        };
        return passes;
    }

    void end_meeting() { engine->set_active(false); }

    fv::testkit::TempDataRoot root;
    fv::FakeClock clock{fixture_now()};
    ms::testing::FixedIds ids;
    std::shared_ptr<FakeWebRepo> repo = std::make_shared<FakeWebRepo>();
    std::shared_ptr<ms::MeetingStateStore> store;
    std::unique_ptr<mw::testing::FakeDashboardEngine> engine;
    std::unique_ptr<ApiServer> app;
};

// A 16 kHz mono 16-bit WAV of `samples` silent samples.
void write_wav(const std::filesystem::path& path, std::uint32_t samples) {
    std::ofstream out(path, std::ios::binary);
    const auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.put(static_cast<char>((v >> (8 * i)) & 0xFF));
        }
    };
    const auto put16 = [&](std::uint16_t v) {
        out.put(static_cast<char>(v & 0xFF));
        out.put(static_cast<char>(v >> 8));
    };
    out.write("RIFF", 4);
    put32(36 + samples * 2);
    out.write("WAVEfmt ", 8);
    put32(16);
    put16(1);
    put16(1);
    put32(16000);
    put32(32000);
    put16(2);
    put16(16);
    out.write("data", 4);
    put32(samples * 2);
    for (std::uint32_t i = 0; i < samples; ++i) {
        put16(0);
    }
}

} // namespace

// ---- TestDashboardPage ----

// from tests/test_meeting_web_auth.py::TestDashboardPage::test_serves_the_built_bundle
TEST_CASE("test_serves_the_built_bundle", "[test_meeting_web_auth]") {
    fv::testkit::TempDataRoot dist;
    {
        std::ofstream index(dist.path() / "index.html", std::ios::binary);
        index << "<div id=root></div>";
    }
    WebClient web(WebClient::default_passes(), dist.path());
    const auto r = web.app->get("/m/" + kGuest);
    CHECK(r.status == 200);
    CHECK(r.body == "<div id=root></div>");
    CHECK(r.header("content-type") == "text/html; charset=utf-8");
}

// from tests/test_meeting_web_auth.py::TestDashboardPage::test_missing_bundle_says_how_to_build_it
TEST_CASE("test_missing_bundle_says_how_to_build_it", "[test_meeting_web_auth]") {
    WebClient web;
    const auto r = web.app->get("/m/" + kHost);
    const auto rejected = web.app->get("/m/not-a-token");
    CHECK(r.status == 200);
    CHECK(r.header("content-type").value_or("").starts_with("text/html"));
    CHECK(contains(r.body, "npm run build --prefix webui"));
    CHECK(contains(r.body, "<title>" + std::string(fv::identity().product) + " Meeting</title>"));
    CHECK(rejected.status == 403);
    CHECK(rejected.body == R"({"detail":"invalid or expired link"})");
}

TEST_CASE("the vendored bundle is served from the dashboard root", "[dashboard]") {
    const auto bundle = fv::testkit::source_root() / "webui" / "dist";
    WebClient web(WebClient::default_passes(), bundle);
    const auto page = web.app->get("/m/" + kHost);
    CHECK(page.status == 200);
    std::ifstream in(bundle / "index.html", std::ios::binary);
    const std::string index((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(page.body == index);
    std::string script;
    for (const auto& entry : std::filesystem::directory_iterator(bundle / "assets")) {
        if (entry.path().extension() == ".js") {
            script = entry.path().filename().string();
        }
    }
    REQUIRE_FALSE(script.empty());
    const auto asset = web.app->get("/assets/" + script);
    CHECK(asset.status == 200);
    CHECK(asset.header("content-type") == "text/javascript; charset=utf-8");
    // StaticFiles answers a conditional request with 304.
    const auto again = web.app->get("/assets/" + script, {},
                                    {{"If-None-Match", asset.header("etag").value_or("")}});
    CHECK(again.status == 304);
    CHECK(web.app->get("/assets/../index.html").status == 404);
    CHECK(web.app->get("/assets/missing.js").body == R"({"detail":"Not Found"})");
    // A stale link is refused; the bundle's licences are not served.
    CHECK(web.app->get("/m/stale-token").body == R"({"detail":"invalid or expired link"})");
    CHECK(web.app->get("/licenses/OFL.txt").status == 404);
}

// ---- TestHostOnlyAuthz ----

// from tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_guest_cannot_list_meetings
TEST_CASE("test_guest_cannot_list_meetings", "[test_meeting_web_auth]") {
    WebClient web;
    const auto r = web.app->get("/api/meetings", kGuestQuery);
    CHECK(r.status == 403);
    CHECK(r.json()["detail"] == "host only");
}

// from tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_host_can_list_meetings
TEST_CASE("test_host_can_list_meetings", "[test_meeting_web_auth]") {
    WebClient web;
    const auto r = web.app->get("/api/meetings", kHostQuery);
    REQUIRE(r.status == 200);
    const Json meeting = r.json()["meetings"][0];
    CHECK(meeting["id"] == "m_test");
    CHECK(meeting["display_title"] == "Auth Test");
    CHECK(meeting["has_audio"] == true);
    CHECK(meeting["can_rerun_speakers"] == true);
    // Tokens must not leak in public meeting payloads.
    CHECK_FALSE(meeting.contains("host_token"));
    CHECK_FALSE(meeting.contains("guest_token"));
}

// from
// tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_meeting_list_uses_topic_when_no_title_was_set
TEST_CASE("test_meeting_list_uses_topic_when_no_title_was_set", "[test_meeting_web_auth]") {
    WebClient web;
    web.repo->set_meetings({Json{{"id", "m_history"},
                                 {"title", ""},
                                 {"status", "ended"},
                                 {"started_at", "2026-01-02T00:00:00"},
                                 {"state_json", R"({"topic":{"current":"Quarterly roadmap"}})"}}});
    const auto response = web.app->get("/api/meetings", kHostQuery);
    REQUIRE(response.status == 200);
    const Json meeting = response.json()["meetings"][0];
    CHECK(meeting["title"] == "");
    CHECK(meeting["display_title"] == "Quarterly roadmap");
    CHECK_FALSE(meeting.contains("state_json"));
    CHECK(meeting.contains("finalization_status"));
    CHECK(meeting.contains("finalization_deferred"));
}

// from
// tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_meeting_list_exposes_compact_finalization_fields
TEST_CASE("test_meeting_list_exposes_compact_finalization_fields", "[test_meeting_web_auth]") {
    WebClient web;
    web.repo->set_meetings(
        {Json{{"id", "m_saved"},
              {"title", "Saved meeting"},
              {"status", "ended"},
              {"started_at", "2026-01-02T00:00:00"},
              {"cloud_enabled", true},
              {"state_json", R"({"meeting_id":"m_saved","status":"ended","cloud_enabled":true,)"
                             R"("finalization":{"status":"failed","message":"interrupted",)"
                             R"("card_deferred":true}})"}}});
    const auto response = web.app->get("/api/meetings", kHostQuery);
    REQUIRE(response.status == 200);
    const Json meeting = response.json()["meetings"][0];
    CHECK(meeting["finalization_status"] == "failed");
    CHECK(meeting["finalization_deferred"] == true);
    CHECK(meeting["insights_pill"] == "Saved for later");
    CHECK(meeting["insights_tone"] == "warning");
    CHECK_FALSE(meeting.contains("state_json"));
    CHECK_FALSE(meeting.contains("steps"));
    CHECK_FALSE(meeting.contains("host_token"));
}

// from
// tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_meeting_list_exposes_duration_and_digest
TEST_CASE("test_meeting_list_exposes_duration_and_digest", "[test_meeting_web_auth]") {
    WebClient web;
    const Json state = Json::parse(R"({
        "participants": {
            "p2": {"id": "p2", "display_name": "Maya Chen", "kind": "guest",
                   "created_at": "2026-01-02T09:01:00Z"},
            "me": {"id": "me", "display_name": "Me", "kind": "me",
                   "created_at": "2026-01-02T09:05:00Z"}},
        "cards": {
            "decisions": [{"id": "d1", "status": "confirmed"}, {"id": "d2", "status": "removed"}],
            "action_items": [{"id": "a1", "status": "proposed"}, {"id": "a2", "status": "edited"}],
            "risks": []},
        "questions": [{"id": "q1", "status": "open"}, {"id": "q2", "status": "resolved"}]})");
    web.repo->set_meetings({Json{{"id", "m_digest"},
                                 {"title", "Scope review"},
                                 {"status", "ended"},
                                 {"started_at", "2026-01-02T09:00:00Z"},
                                 {"ended_at", "2026-01-02T09:50:00Z"},
                                 {"paused_total_s", 180},
                                 {"state_json", state.dump()}}});
    const Json meeting = web.app->get("/api/meetings", kHostQuery).json()["meetings"][0];
    CHECK(meeting["duration_s"] == 50 * 60 - 180);
    const Json digest = meeting["digest"];
    CHECK(digest["decisions"] == 1); // removed cards are not counted
    CHECK(digest["action_items"] == 2);
    CHECK(digest["risks"] == 0);
    CHECK(digest["open_questions"] == 1);
    CHECK(digest["participant_count"] == 2);
    CHECK(digest["participants"][0]["id"] == "me"); // me leads
    CHECK(digest["participants"][1]["id"] == "p2");
    CHECK_FALSE(meeting.contains("state_json"));
}

// from
// tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_meeting_list_survives_corrupt_snapshot
TEST_CASE("test_meeting_list_survives_corrupt_snapshot", "[test_meeting_web_auth]") {
    WebClient web;
    web.repo->set_meetings({Json{{"id", "m_corrupt"},
                                 {"title", "Broken"},
                                 {"status", "ended"},
                                 {"started_at", "not a date"},
                                 {"state_json", "{not json"}}});
    const auto response = web.app->get("/api/meetings", kHostQuery);
    REQUIRE(response.status == 200);
    const Json meeting = response.json()["meetings"][0];
    CHECK(meeting["duration_s"].is_null());
    CHECK_FALSE(meeting.contains("digest"));
}

// from tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_guest_cannot_end_meeting
TEST_CASE("test_guest_cannot_end_meeting", "[test_meeting_web_auth]") {
    WebClient web;
    CHECK(web.app->post("/api/meeting/end", kGuestQuery).status == 403);
    CHECK(web.engine->calls("end").empty());
}

// from tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_host_can_end_meeting
TEST_CASE("test_host_can_end_meeting", "[test_meeting_web_auth]") {
    WebClient web;
    const auto r = web.app->post("/api/meeting/end", kHostQuery);
    CHECK(r.status == 200);
    CHECK(r.body == R"({"ok":true})");
    CHECK(web.engine->calls("end").size() == 1);
}

// from
// tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_slow_end_is_logged_with_where_the_time_went
TEST_CASE("test_slow_end_is_logged_with_where_the_time_went", "[test_meeting_web_auth]") {
    WebClient web;
    // engine.end() takes two seconds of the test's clock.
    web.engine->set_lifecycle_hook([&web](std::string_view method, const Json&) {
        if (method == "end") {
            web.clock.advance(2s);
        }
    });
    ScopedLogCapture logs;
    const auto r = web.app->post("/api/meeting/end", kHostQuery);
    CHECK(r.status == 200);
    CHECK(web.engine->calls("end").size() == 1);
    bool logged = false;
    for (const auto& line : logs.capture->lines()) {
        logged = logged || (line.starts_with("meeting.web.api: ") &&
                            contains(line, "Meeting end request took 2.00s") &&
                            contains(line, "waiting for a worker thread") &&
                            contains(line, "2.00s in engine.end"));
    }
    CHECK(logged);
}

// from tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_quick_end_is_not_logged
TEST_CASE("test_quick_end_is_not_logged", "[test_meeting_web_auth]") {
    WebClient web;
    ScopedLogCapture logs;
    web.app->post("/api/meeting/end", kHostQuery);
    for (const auto& line : logs.capture->lines()) {
        CHECK_FALSE(contains(line, "Meeting end request took"));
    }
}

// from tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_guest_cannot_search_or_export
TEST_CASE("test_guest_cannot_search_or_export", "[test_meeting_web_auth]") {
    WebClient web;
    CHECK(web.app->get("/api/search", {{"token", kGuest}, {"q", "x"}}).status == 403);
    CHECK(web.app->get("/api/export/md", kGuestQuery).status == 403);
}

// from tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_host_can_search
TEST_CASE("test_host_can_search", "[test_meeting_web_auth]") {
    WebClient web;
    const auto r = web.app->get("/api/search", {{"token", kHost}, {"q", "budget"}});
    CHECK(r.status == 200);
    CHECK(web.repo->search_calls() == std::vector<std::string>{"budget"});
    CHECK(r.body == R"({"results":[],"mode":"keyword","message":""})");
}

// from tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_invalid_token_is_401
TEST_CASE("test_invalid_token_is_401", "[test_meeting_web_auth]") {
    WebClient web;
    const auto r = web.app->get("/api/session", {{"token", "wrong"}});
    CHECK(r.status == 401);
    CHECK(r.body == R"({"detail":"invalid token"})");
}

// from tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_guest_can_read_session
TEST_CASE("test_guest_can_read_session", "[test_meeting_web_auth]") {
    WebClient web;
    const auto r = web.app->get("/api/session", kGuestQuery);
    REQUIRE(r.status == 200);
    CHECK(r.json()["role"] == "guest");
    CHECK_FALSE(r.json()["meeting"].contains("host_token"));
}

// from
// tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_transcript_keyset_pagination_has_no_gaps
TEST_CASE("test_transcript_keyset_pagination_has_no_gaps", "[test_meeting_web_auth]") {
    WebClient web;
    std::vector<Json> segments;
    for (int i = 0; i < 3; ++i) {
        segments.push_back(Json{{"id", "sg_" + std::to_string(i)},
                                {"meeting_id", "m_test"},
                                {"start_s", 1.0},
                                {"end_s", 2.0},
                                {"text", std::to_string(i)},
                                {"channel", "mic"}});
    }
    web.repo->set_segments(segments);
    const auto first = web.app->get("/api/transcript", {{"token", kHost}, {"limit", "2"}});
    REQUIRE(first.status == 200);
    CHECK(first.json()["items"][0]["id"] == "sg_0");
    CHECK(first.json()["items"][1]["id"] == "sg_1");
    CHECK(first.json()["items"].size() == 2);
    const std::string cursor = first.json()["next_cursor"].get<std::string>();
    CHECK_FALSE(cursor.empty());
    const auto second =
        web.app->get("/api/transcript", {{"token", kHost}, {"limit", "2"}, {"cursor", cursor}});
    CHECK(second.json()["items"].size() == 1);
    CHECK(second.json()["items"][0]["id"] == "sg_2");
    CHECK(second.json()["next_cursor"].is_null());
}

// from
// tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_guest_segment_access_is_scoped_to_current_meeting
TEST_CASE("test_guest_segment_access_is_scoped_to_current_meeting", "[test_meeting_web_auth]") {
    WebClient web;
    web.repo->set_segments({Json{{"id", "sg_1"},
                                 {"meeting_id", "m_test"},
                                 {"start_s", 1.0},
                                 {"end_s", 2.0},
                                 {"text", "hello"},
                                 {"channel", "mic"}}});
    CHECK(web.app->get("/api/meetings/m_test/segments/sg_1", kGuestQuery).status == 200);
    const auto blocked = web.app->get("/api/meetings/m_other/segments/sg_1", kGuestQuery);
    CHECK(blocked.status == 403);
    CHECK(blocked.json()["detail"] == "guest access is current meeting only");
}

// from
// tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_historical_state_is_upgraded_to_current_wire_shape
TEST_CASE("test_historical_state_is_upgraded_to_current_wire_shape", "[test_meeting_web_auth]") {
    WebClient web;
    web.engine->set_store(nullptr); // the persisted-state branch
    const auto response = web.app->get("/api/meetings/m_test", kHostQuery);
    REQUIRE(response.status == 200);
    const Json state = response.json()["state"];
    CHECK(state["meeting_id"] == "m_test");
    CHECK(state["rolling_summary_evidence"] == Json::array());
    CHECK(state["capture"] == Json{{"mic_available", false},
                                   {"loopback_available", false},
                                   {"message", ""},
                                   {"audio_incomplete", false},
                                   {"integrity_error", ""}});
}

// from
// tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_activity_and_token_rotation_are_host_only
TEST_CASE("test_activity_and_token_rotation_are_host_only", "[test_meeting_web_auth]") {
    WebClient web;
    CHECK(web.app->get("/api/events", kGuestQuery).status == 403);
    const auto events = web.app->get("/api/events", kHostQuery);
    REQUIRE(events.status == 200);
    CHECK(events.json()["events"][0]["undoable"] == true);
    CHECK(web.app->post("/api/meeting/tokens/regenerate", kGuestQuery).status == 403);
    const auto rotated = web.app->post("/api/meeting/tokens/regenerate", kHostQuery);
    REQUIRE(rotated.status == 200);
    CHECK(rotated.json()["host_url"] == "/m/new-host");
    CHECK(rotated.body == R"({"ok":true,"host_url":"/m/new-host","guest_url":"/m/new-guest"})");
    CHECK(web.engine->calls("regenerate_tokens").size() == 1);
}

// from
// tests/test_meeting_web_auth.py::TestHostOnlyAuthz::test_audio_allows_current_guest_but_not_other_meetings
TEST_CASE("test_audio_allows_current_guest_but_not_other_meetings", "[test_meeting_web_auth]") {
    WebClient web;
    // A real recording in place of the monkeypatched build_playback: one chunk in the spool.
    const auto spool = web.root.path() / "spool";
    std::filesystem::create_directories(spool);
    write_wav(spool / "mic_00000.wav", 1600);
    Json meeting = fake_web_meeting();
    meeting["spool_dir"] = fv::path_to_utf8(spool);
    web.repo->set_meeting(meeting);
    web.repo->set_chunks({Json{{"id", 1},
                               {"meeting_id", "m_test"},
                               {"channel", "mic"},
                               {"seq", 0},
                               {"start_s", 0.0},
                               {"duration_s", 0.1},
                               {"sample_rate", 16000},
                               {"file_path", fv::path_to_utf8(spool / "mic_00000.wav")}}});
    const auto allowed = web.app->get("/api/meetings/m_test/audio", kGuestQuery);
    CHECK(allowed.status == 200);
    CHECK(allowed.header("cache-control") == "no-store");
    CHECK(allowed.header("content-type") == "audio/wav");
    CHECK(allowed.header("content-disposition") == "inline; filename=\"meeting-m_test.wav\"");
    const auto partial =
        web.app->get("/api/meetings/m_test/audio", kHostQuery, {{"Range", "bytes=0-3"}});
    CHECK(partial.status == 206);
    CHECK(partial.body == "RIFF");
    CHECK(web.app->get("/api/meetings/m_other/audio", kGuestQuery).status == 403);
}

TEST_CASE("a meeting without audio answers the playback error", "[dashboard]") {
    WebClient web;
    const auto missing = web.app->get("/api/meetings/m_test/audio", kHostQuery);
    CHECK(missing.status == 404);
    CHECK(missing.body == R"({"detail":"meeting audio is unavailable"})");
    const auto unknown = web.app->get("/api/meetings/m_gone/audio", kHostQuery);
    CHECK(unknown.status == 400);
    CHECK(unknown.body == R"({"detail":"unknown meeting"})");
}

// ---- The re-run guards ----

// from tests/test_meeting_web_auth.py::test_rerun_endpoints_share_their_guards
TEST_CASE("test_rerun_endpoints_share_their_guards", "[test_meeting_web_auth]") {
    const std::string endpoint = GENERATE("respeakers", "reinsights");
    struct Case {
        std::string meeting_id, token;
        bool ended;
        int status;
    };
    const Case c = GENERATE(Case{"m_test", kGuest, false, 403}, Case{"m_test", kHost, false, 409},
                            Case{"m_gone", kHost, true, 404});
    WebClient web;
    if (c.ended) {
        web.end_meeting();
    }
    const auto r =
        web.app->post("/api/meetings/" + c.meeting_id + "/" + endpoint, {{"token", c.token}});
    CHECK(r.status == c.status);
}

// from tests/test_meeting_web_auth.py::TestRerunSpeakers::test_non_openai_backend_is_400
TEST_CASE("test_non_openai_backend_is_400", "[test_meeting_web_auth]") {
    const std::string backend = GENERATE("local", "off");
    mw::PostMeetingPasses passes = WebClient::default_passes();
    std::atomic<int> reruns{0};
    passes.rerun_speakers = [&reruns](const std::string&, std::shared_ptr<ms::MeetingStateStore>) {
        ++reruns;
        return Json::object();
    };
    // resolve_speaker_pass's refusal for a backend that is not OpenAI.
    passes.speaker_gate_refusal = [backend](const Json&) -> std::optional<std::string> {
        if (backend != "openai") {
            return "Speaker identification is not set to OpenAI.";
        }
        return std::nullopt;
    };
    WebClient web(std::move(passes));
    web.end_meeting();
    const auto r = web.app->post("/api/meetings/m_test/respeakers", kHostQuery);
    CHECK(r.status == 400);
    CHECK(r.json()["detail"] == "Speaker identification is not set to OpenAI.");
    CHECK(reruns == 0);
}

// from tests/test_meeting_web_auth.py::TestRerunSpeakers::test_meeting_without_system_audio_is_400
TEST_CASE("test_meeting_without_system_audio_is_400", "[test_meeting_web_auth]") {
    WebClient web;
    web.end_meeting();
    web.repo->set_chunks({});
    const auto r = web.app->post("/api/meetings/m_test/respeakers", kHostQuery);
    CHECK(r.status == 400);
    CHECK(contains(r.json()["detail"].get<std::string>(), "no system-audio recording"));
}

// from
// tests/test_meeting_web_auth.py::TestRerunSpeakers::test_rerun_waits_for_other_post_meeting_steps
TEST_CASE("test_rerun_waits_for_other_post_meeting_steps", "[test_meeting_web_auth]") {
    // The dashboard's speaker pass takes the same per-meeting claim as a retry: the pass
    // (refinalize's rerun_speakers) refuses while another holds it, before any upload.
    mw::PostMeetingPasses passes = WebClient::default_passes();
    passes.speaker_gate_refusal = [](const Json&) {
        return std::optional<std::string>();
    };
    passes.rerun_speakers = [](const std::string&, std::shared_ptr<ms::MeetingStateStore>) -> Json {
        throw mw::PostMeetingPassBusy("post-meeting steps are already running for this meeting");
    };
    WebClient web(std::move(passes));
    web.end_meeting();
    const auto r = web.app->post("/api/meetings/m_test/respeakers", kHostQuery);
    CHECK(r.status == 409);
    CHECK(r.json()["detail"] == "post-meeting steps are already running for this meeting");
}

TEST_CASE("the speaker re-run passes the meeting's store and answers its result", "[dashboard]") {
    std::shared_ptr<ms::MeetingStateStore> seen;
    std::string seen_id;
    mw::PostMeetingPasses passes = WebClient::default_passes();
    passes.rerun_speakers = [&](const std::string& meeting_id,
                                std::shared_ptr<ms::MeetingStateStore> store) {
        seen_id = meeting_id;
        seen = std::move(store);
        return Json{{"ok", true}, {"relabeled", 3}};
    };
    WebClient web(std::move(passes));
    web.end_meeting();
    const auto r = web.app->post("/api/meetings/m_test/respeakers", kHostQuery);
    CHECK(r.status == 200);
    CHECK(r.body == R"({"ok":true,"relabeled":3})");
    CHECK(seen_id == "m_test");
    // A just-ended current meeting is relabelled through the engine's live store.
    CHECK(seen == web.store);
}

// from
// tests/test_meeting_web_auth.py::TestRerunInsights::test_rerun_uses_shared_options_and_the_host_lease
TEST_CASE("test_rerun_uses_shared_options_and_the_host_lease", "[test_meeting_web_auth]") {
    // The options (rerun_options) and the model lease are the injected pass's own (the meeting
    // runtime builds it over meeting::rerun_finalization); the route asks for the failed steps,
    // off the request's thread, and adopts the result's state into the meeting's store.
    std::string from_step;
    std::thread::id pass_thread;
    mw::PostMeetingPasses passes = WebClient::default_passes();
    passes.rerun_insights = [&](const std::string& meeting_id, const std::string& step,
                                const fv::CancelToken&) {
        CHECK(meeting_id == "m_test");
        from_step = step;
        pass_thread = std::this_thread::get_id();
        return Json{{"ok", true},
                    {"state", Json{{"meeting_id", "m_test"}, {"title", "Adopted"}}},
                    {"applied", 0},
                    {"error", nullptr}};
    };
    WebClient web(std::move(passes));
    web.end_meeting();
    const auto r = web.app->post("/api/meetings/m_test/reinsights", kHostQuery);
    CHECK(r.status == 200);
    CHECK(from_step == "failed");
    CHECK(pass_thread != std::this_thread::get_id());
    CHECK(web.store->snapshot()["title"] == "Adopted");
}

// from
// tests/test_meeting_web_auth.py::TestRerunInsights::test_retry_already_running_elsewhere_is_409
TEST_CASE("test_retry_already_running_elsewhere_is_409", "[test_meeting_web_auth]") {
    mw::PostMeetingPasses passes = WebClient::default_passes();
    passes.rerun_insights = [](const std::string&, const std::string&,
                               const fv::CancelToken&) -> Json {
        throw mw::PostMeetingPassBusy("already running");
    };
    WebClient web(std::move(passes));
    web.end_meeting();
    const auto r = web.app->post("/api/meetings/m_test/reinsights", kHostQuery);
    CHECK(r.status == 409);
    CHECK(r.body == R"({"detail":"already running"})");
}

// from tests/test_meeting_web_auth.py::TestRerunInsights::test_missing_transcript_is_400. As in
// Python the pass is replaced by one that raises; the real re-run records "meeting has no
// transcript" on its consolidation step and raises only for an unknown meeting.
TEST_CASE("test_missing_transcript_is_400", "[test_meeting_web_auth]") {
    mw::PostMeetingPasses passes = WebClient::default_passes();
    passes.rerun_insights = [](const std::string&, const std::string&,
                               const fv::CancelToken&) -> Json {
        throw fv::py::ValueError("meeting has no transcript");
    };
    WebClient web(std::move(passes));
    web.end_meeting();
    const auto r = web.app->post("/api/meetings/m_test/reinsights", kHostQuery);
    CHECK(r.status == 400);
    CHECK(r.json()["detail"] == "meeting has no transcript");
}

// from tests/test_meeting_web_auth.py::TestRerunInsights::test_live_end_still_finalizing_is_409
TEST_CASE("test_live_end_still_finalizing_is_409", "[test_meeting_web_auth]") {
    std::atomic<int> reruns{0};
    mw::PostMeetingPasses passes = WebClient::default_passes();
    passes.rerun_insights = [&reruns](const std::string&, const std::string&,
                                      const fv::CancelToken&) {
        ++reruns; // must not start a second pipeline
        return Json::object();
    };
    ms::MeetingState state = fake_web_state();
    state.finalization.status = "running";
    WebClient web(std::move(passes), std::nullopt, std::move(state));
    web.end_meeting();
    const auto r = web.app->post("/api/meetings/m_test/reinsights", kHostQuery);
    CHECK(r.status == 409);
    CHECK(r.json()["detail"] == "Wait for the post-meeting steps to finish");
    CHECK(reruns == 0);
}

// from tests/test_meeting_web_auth.py::TestRerunInsights::test_second_rerun_while_running_is_409
TEST_CASE("test_second_rerun_while_running_is_409", "[test_meeting_web_auth]") {
    // A double-click must not start two agent cores on one meeting.
    std::promise<void> running;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::atomic<int> calls{0};
    mw::PostMeetingPasses passes = WebClient::default_passes();
    passes.rerun_insights = [&](const std::string&, const std::string&, const fv::CancelToken&) {
        if (calls++ == 0) {
            running.set_value();
            released.wait_for(10s);
        }
        return Json{{"ok", true},
                    {"state", Json{{"meeting_id", "m_test"}, {"seq", 1}}},
                    {"applied", 0},
                    {"error", nullptr}};
    };
    WebClient web(std::move(passes));
    web.end_meeting();
    std::optional<HttpResult> first;
    std::thread worker(
        [&] { first = web.app->post("/api/meetings/m_test/reinsights", kHostQuery); });
    REQUIRE(running.get_future().wait_for(10s) == std::future_status::ready);

    const auto second = web.app->post("/api/meetings/m_test/reinsights", kHostQuery);
    CHECK(second.status == 409);
    CHECK(second.json()["detail"] == "insights are already running for this meeting");

    release.set_value();
    worker.join();
    REQUIRE(first.has_value());
    CHECK(first->status == 200);
    // The guard releases: a later run is accepted again.
    CHECK(web.app->post("/api/meetings/m_test/reinsights", kHostQuery).status == 200);
}

// ---- TestExportTokenStripping ----

// from
// tests/test_meeting_web_auth.py::TestExportTokenStripping::test_export_never_carries_capability_tokens
TEST_CASE("test_export_never_carries_capability_tokens", "[test_meeting_web_auth]") {
    const std::string fmt = GENERATE("md", "json", "txt");
    WebClient web;
    const auto r = web.app->get("/api/export/" + fmt, kHostQuery);
    REQUIRE(r.status == 200);
    CHECK_FALSE(contains(r.body, kHost));
    CHECK_FALSE(contains(r.body, kGuest));
    CHECK_FALSE(contains(r.body, "host_token"));
    CHECK_FALSE(contains(r.body, "guest_token"));
    CHECK(r.header("content-disposition") == "attachment; filename=\"meeting-m_test." + fmt + "\"");
}

// from tests/test_meeting_web_auth.py::TestExportTokenStripping::test_unknown_export_format_is_404
TEST_CASE("test_unknown_export_format_is_404", "[test_meeting_web_auth]") {
    WebClient web;
    const auto r = web.app->get("/api/export/pdf", kHostQuery);
    CHECK(r.status == 404);
    CHECK(r.body == R"({"detail":"unknown export format"})");
}

// ---- tests/test_meeting_export_features.py ----

// from
// tests/test_meeting_export_features.py::test_web_downloads_preserve_current_features_and_match_bulk_content
TEST_CASE("test_web_downloads_preserve_current_features_and_match_bulk_content",
          "[test_meeting_export_features]") {
    const std::string fmt = GENERATE("md", "json", "txt");
    // _recent_features(): the export fixture with review, citation, pulse and note features.
    ms::test_support::ExportFixture f;
    Json& action = f.state["cards"]["action_items"][0];
    action["revision"] = 2;
    action["review"] = {{"state", "human"}};
    action["citation_check"] = {{"status", "contradicted"}, {"revision", 2}};
    action["data"]["deadline"] = "Friday";
    f.state["insight_review"] = Json::parse(R"({"questions": [
        {"correction": "The RFC is due Friday.", "status": "answered"},
        {"correction": "OUTDATED CLARIFICATION", "superseded": true}]})");
    f.state["live_highlights"] = Json::parse(R"([
        {"id": "late", "kind": "number", "start_s": 90, "text": "Budget is 5000",
         "segment_id": "sg_2", "probability": 0.9,
         "assessment": {"threshold": 0.8, "scores": {"number": 0.9}}},
        {"id": "early", "kind": "decision", "start_s": 5, "text": "Choose June",
         "segment_id": "sg_1", "probability": 0.95}])");
    f.state["meeting_id"] = "m_test";
    auto state = ms::MeetingState::from_json(f.state, f.clock);

    WebClient web(WebClient::default_passes(), std::nullopt, std::move(state));
    const Json snapshot = web.store->snapshot();
    Json meeting = fake_web_meeting();
    meeting["state_json"] = snapshot.dump();
    meeting["state_seq"] = 9;
    meeting["agent_provider"] = "ollama";
    meeting["agent_model"] = "qwen";
    meeting["agent_endpoint_json"] = R"({"base_url":"http://localhost:11434/v1"})";
    web.repo->set_meeting(meeting);
    std::vector<Json> segments;
    for (Json segment : f.segments) {
        segment["meeting_id"] = "m_test";
        segments.push_back(std::move(segment));
    }
    web.repo->set_segments(segments);

    const auto response = web.app->get("/api/export/" + fmt, kHostQuery);
    REQUIRE(response.status == 200);
    CHECK(contains(response.header("content-disposition").value_or(""), "attachment;"));
    const auto entry = ms::collect_meeting_export(*web.repo, "m_test");
    REQUIRE(entry.has_value());
    if (fmt == "json") {
        const Json payload = response.json();
        CHECK(payload["state"] == web.store->snapshot());
        for (const char* key :
             {"agent_provider", "agent_model", "agent_endpoint_json", "state_seq"}) {
            CHECK(payload["meeting"][key] == entry->meeting[key]);
        }
        CHECK_FALSE(payload["meeting"].contains("host_token"));
        CHECK_FALSE(payload["meeting"].contains("guest_token"));
    } else {
        CHECK(response.body ==
              ms::render_meeting_document(*entry, fmt == "md" ? "markdown" : fmt, {}, web.clock));
    }
}

// ---- tests/test_meeting_custom_report.py ----

namespace {

constexpr std::string_view kPast = "m_past";

// _ended_state(): an ended meeting with AI insights whose finalization completed.
ms::MeetingState ended_state() {
    ms::MeetingState state;
    state.meeting_id = std::string(kPast);
    state.title = "Vendor renewal";
    state.status = "ended";
    state.cloud_enabled = true;
    state.finalization =
        ms::FinalizationState::coerce(Json{{"status", "completed"}}, true, "ended");
    return state;
}

// ReportRepo's ended meeting beside the active one.
Json past_meeting() {
    return Json{{"id", kPast},
                {"title", "Vendor renewal"},
                {"started_at", "2026-09-19T10:00:00Z"},
                {"cloud_enabled", true},
                {"status", "ended"},
                {"ended_at", "2026-09-19T11:00:00Z"},
                {"paused_total_s", 0.0},
                {"asr_model", "base"},
                {"host_token", kFakeHostToken},
                {"guest_token", kFakeGuestToken},
                {"agent_provider", "openrouter"},
                {"agent_model", "test-model"},
                {"state_json", ended_state().to_json().dump()}};
}

// The api_client fixture's start_custom_report, through the extension seam: the host's request
// claims a slot on the meeting's owning store and, unless the test leaves it running, the worker
// publishes "# Brief" at once.
class ReportExtension final : public mw::DashboardExtensions {
public:
    std::atomic<bool> leave_running{false};
    std::atomic<int> started{0};

    mw::DashboardResponse review(mw::DashboardApi&, const std::string&, const Json&) override {
        throw mw::HttpError(409, "unused");
    }
    mw::DashboardResponse request_report(mw::DashboardApi& api, const std::string& meeting_id,
                                         const std::string& request) override {
        const Json meeting = api.saved_meeting(meeting_id);
        if (api.is_claimed(meeting_id)) {
            throw mw::HttpError(409, "Wait for the meeting's insights to finish");
        }
        const auto store = api.owning_store(meeting_id, meeting);
        ++started;
        const std::string run_id = leave_running ? "run_held" : "run_" + std::to_string(started);
        const auto results = store->apply(
            "host", std::nullopt,
            Json::array(
                {Json{{"op", "request_custom_report"}, {"request", request}, {"run_id", run_id}}}));
        if (results.empty() || !results[0].ok) {
            throw mw::HttpError(409, "A report is already being written for this meeting.");
        }
        const std::string report_id = results[0].target_id.value_or("");
        if (!leave_running) {
            store->apply("system", std::string(ms::WORKER_ACTOR),
                         Json::array({Json{{"op", "finish_custom_report"},
                                           {"report_id", report_id},
                                           {"run_id", run_id},
                                           {"status", "ready"},
                                           {"markdown", "# Brief\n\nHeld at twelve."},
                                           {"title", "Brief"},
                                           {"sources", Json{{"transcript_lines", 4}}}}}));
        }
        return mw::DashboardResponse::json(Json{{"ok", true},
                                                {"report_id", report_id},
                                                {"error", nullptr},
                                                {"state", store->snapshot()}});
    }
    mw::DashboardResponse request_note(mw::DashboardApi&, const std::string&,
                                       const std::string&) override {
        throw mw::HttpError(409, "unused");
    }
    mw::DashboardResponse set_cloud(mw::DashboardApi&, bool) override {
        throw mw::HttpError(409, "unused");
    }
    Json semantic_search(mw::DashboardApi&, std::string_view) override { return Json::object(); }
};

struct ReportClient {
    ReportClient() {
        web.repo->set_past(past_meeting());
        web.app->api->set_extensions(extension);
    }

    std::string url() const { return "/api/meetings/" + std::string(kPast) + "/reports"; }
    Json request_report() {
        const auto r = web.app->post_json(url(), kHostQuery, Json{{"request", "every commitment"}});
        REQUIRE(r.status == 200);
        return r.json();
    }

    WebClient web;
    std::shared_ptr<ReportExtension> extension = std::make_shared<ReportExtension>();
};

Json running_report(const std::string& id) {
    return Json{{"id", id},
                {"request", "x"},
                {"status", "running"},
                {"run_id", "run_1"},
                {"markdown", ""},
                {"title", ""},
                {"message", ""},
                {"sources", Json::object()},
                {"requested_by", nullptr},
                {"created_at", "2026-09-19T11:00:00Z"},
                {"updated_at", "2026-09-19T11:00:00Z"}};
}

} // namespace

// from
// tests/test_meeting_custom_report.py::TestApi::test_an_interrupted_run_is_shown_as_failed_not_still_running
TEST_CASE("test_an_interrupted_run_is_shown_as_failed_not_still_running",
          "[test_meeting_custom_report]") {
    ReportClient client;
    Json past = client.web.repo->past();
    Json state = Json::parse(past["state_json"].get<std::string>());
    state["custom_reports"] = Json::array({running_report("rep_dead")});
    past["state_json"] = state.dump();
    client.web.repo->set_past(past);
    const auto detail = client.web.app->get("/api/meetings/m_past", kHostQuery);
    REQUIRE(detail.status == 200);
    const Json report = detail.json()["state"]["custom_reports"][0];
    CHECK(report["status"] == "failed");
    CHECK(contains(report["message"].get<std::string>(), "interrupted"));
}

// from
// tests/test_meeting_custom_report.py::TestApi::test_delete_removes_a_report_and_reports_unknown_ids
TEST_CASE("test_delete_removes_a_report_and_reports_unknown_ids", "[test_meeting_custom_report]") {
    ReportClient client;
    const Json created = client.request_report();
    const std::string url = client.url() + "/" + created["report_id"].get<std::string>();
    auto& app = *client.web.app;
    CHECK(app.del(url).status == 401);
    CHECK(app.del(url, kGuestQuery).status == 403);
    const auto response = app.del(url, kHostQuery);
    REQUIRE(response.status == 200);
    CHECK(response.json()["state"]["custom_reports"] == Json::array());
    const auto again = app.del(url, kHostQuery);
    CHECK(again.status == 404);
    CHECK(again.body == R"({"detail":"unknown_report"})");
}

// from tests/test_meeting_custom_report.py::TestApi::test_download_serves_the_markdown_as_a_file
TEST_CASE("test_download_serves_the_markdown_as_a_file", "[test_meeting_custom_report]") {
    ReportClient client;
    const Json created = client.request_report();
    const std::string url =
        client.url() + "/" + created["report_id"].get<std::string>() + "/download";
    auto& app = *client.web.app;
    CHECK(app.get(url).status == 401);
    const auto response = app.get(url, kHostQuery);
    REQUIRE(response.status == 200);
    CHECK(response.body.starts_with("# Brief"));
    CHECK(response.header("content-type").value_or("").starts_with("text/markdown"));
    CHECK(contains(response.header("content-disposition").value_or(""), "attachment"));
    CHECK(app.get(client.url() + "/rep_nope/download", kHostQuery).status == 404);
}

// from
// tests/test_meeting_custom_report.py::TestGuestScoping::test_the_session_endpoint_hides_reports_from_guests
TEST_CASE("test_the_session_endpoint_hides_reports_from_guests", "[test_meeting_custom_report]") {
    ms::MeetingState state = fake_web_state();
    ms::CustomReport secret;
    secret.id = "rep_1";
    secret.markdown = "# secret";
    state.custom_reports.push_back(secret);
    WebClient web(WebClient::default_passes(), std::nullopt, std::move(state));
    for (const auto& [token, visible] : {std::pair{kHost, true}, std::pair{kGuest, false}}) {
        const Json state_json = web.app->get("/api/session", {{"token", token}}).json()["state"];
        CHECK(state_json.contains("custom_reports") == visible);
        CHECK(contains(state_json.dump(), "secret") == visible);
    }
}

// from
// tests/test_meeting_custom_report.py::TestGuestScoping::test_the_meeting_detail_endpoint_is_host_only
TEST_CASE("test_the_meeting_detail_endpoint_is_host_only", "[test_meeting_custom_report]") {
    ReportClient client;
    CHECK(client.web.app->get("/api/meetings/m_past", kGuestQuery).status == 403);
}

// from
// tests/test_meeting_custom_report.py::TestConcurrency::test_a_rerun_waits_for_a_report_in_flight
TEST_CASE("test_a_rerun_waits_for_a_report_in_flight", "[test_meeting_custom_report]") {
    ReportClient client;
    client.extension->leave_running = true;
    client.request_report();
    const auto response = client.web.app->post("/api/meetings/m_past/reinsights", kHostQuery);
    CHECK(response.status == 409);
    CHECK(contains(response.json()["detail"].get<std::string>(), "report being written"));
}

TEST_CASE("host report requests claim persisted work before the report worker completes",
          "[test_meeting_custom_report]") {
    std::promise<void> release;
    auto ready = release.get_future().share();
    std::promise<bool> published;
    auto completed = published.get_future();
    auto passes = WebClient::default_passes();
    passes.report = [ready, &published](auto store, const Json&, std::string_view request,
                                        fv::Executor& worker, const fv::CancelToken& cancel) {
        const auto claimed = store->apply(
            "host", std::nullopt,
            Json::array({Json{
                {"op", "request_custom_report"}, {"request", request}, {"run_id", "run_local"}}}));
        if (claimed.empty() || !claimed.front().ok) {
            return Json{{"ok", false}, {"error", "report_running"}};
        }
        const auto id = claimed.front().target_id.value_or("");
        worker.post([ready, store, id, cancel, &published] {
            if (ready.wait_for(3s) != std::future_status::ready || cancel.cancelled()) {
                return;
            }
            const auto result = store->apply("system", std::string(ms::WORKER_ACTOR),
                                             Json::array({Json{{"op", "finish_custom_report"},
                                                               {"report_id", id},
                                                               {"run_id", "run_local"},
                                                               {"status", "ready"},
                                                               {"markdown", "# Local brief"},
                                                               {"title", "Local brief"}}}));
            published.set_value(!result.empty() && result.front().ok);
        });
        return Json{{"ok", true}, {"report_id", id}, {"error", nullptr}};
    };
    WebClient web(std::move(passes));
    web.repo->set_past(past_meeting());
    const std::string endpoint = "/api/meetings/m_past/reports";
    const Json ask{{"request", "Summarize the decision."}};
    CHECK(web.app->post_json(endpoint, kGuestQuery, ask).status == 403);
    CHECK(web.app->post_json(endpoint, kHostQuery, Json{{"request", " "}}).status == 400);
    const auto response = web.app->post_json(endpoint, kHostQuery, ask);
    REQUIRE(response.status == 200);
    const auto created = response.json();
    CHECK(created.at("state").at("custom_reports").at(0).at("status") == "running");
    CHECK(web.app->post_json(endpoint, kHostQuery, ask).status == 409);
    CHECK(
        web.app->post_json("/api/meetings/m_past/reinsights", kHostQuery, Json::object()).status ==
        409);
    release.set_value();
    REQUIRE(completed.wait_for(3s) == std::future_status::ready);
    REQUIRE(completed.get());
    const auto refreshed = web.app->get("/api/meetings/m_past/state", kHostQuery);
    REQUIRE(refreshed.status == 200);
    const auto report = refreshed.json().at("state").at("custom_reports").at(0);
    CHECK(report.at("status") == "ready");
    const auto download = endpoint + "/" + created.at("report_id").get<std::string>() + "/download";
    CHECK(web.app->get(download, kGuestQuery).status == 403);
    const auto document = web.app->get(download, kHostQuery);
    REQUIRE(document.status == 200);
    CHECK(document.body == "# Local brief");
}

// from
// tests/test_meeting_custom_report.py::TestConcurrency::test_a_finished_report_does_not_block_a_rerun
TEST_CASE("test_a_finished_report_does_not_block_a_rerun", "[test_meeting_custom_report]") {
    const std::string path = GENERATE("reinsights", "respeakers");
    ReportClient client;
    client.request_report();
    const auto response = client.web.app->post("/api/meetings/m_past/" + path, kHostQuery);
    CHECK(response.status != 409);
}

// ---- What M7 answers without the meeting intelligence ----

TEST_CASE("the intelligence routes refuse in plain words without the extensions", "[dashboard]") {
    WebClient web;
    auto& app = *web.app;
    const std::string off = R"({"detail":"Turn on AI insights in an active meeting first."})";
    CHECK(app.post_json("/api/meetings/m_test/review", kHostQuery, Json{{"op", "start"}}).body ==
          off);
    CHECK(app.post_json("/api/meetings/m_test/reports", kHostQuery, Json{{"request", "a brief"}})
              .body == off);
    CHECK(
        app.post_json("/api/meeting/notes/request", kGuestQuery, Json{{"text", "Shorter"}}).body ==
        off);
    // Their validation still comes first.
    CHECK(app.post_json("/api/meetings/m_test/review", kHostQuery, Json{{"op", "nope"}}).status ==
          400);
    CHECK(app.post_json("/api/meeting/notes/request", kHostQuery, Json{{"text", ""}}).status ==
          400);

    // Enabling AI insights needs the desktop consent; disabling reaches the engine and is kept as
    // the next meeting's default.
    const auto enable = app.post_json("/api/meeting/cloud", kHostQuery, Json{{"enabled", true}});
    CHECK(enable.status == 403);
    CHECK(contains(enable.json()["detail"].get<std::string>(), "consent"));
    CHECK(web.engine->calls("set_cloud_enabled").empty());
    app.settings.save_setting(fv::keys::meeting_cloud_last_enabled.key(), true);
    const auto disable = app.post_json("/api/meeting/cloud", kHostQuery, Json{{"enabled", false}});
    CHECK(disable.body == R"({"ok":true,"enabled":false})");
    CHECK(web.engine->calls("set_cloud_enabled").size() == 1);
    CHECK(app.settings.get(fv::keys::meeting_cloud_last_enabled) == false);

    // An engine that cannot switch (the archive) is a clean 409.
    web.engine->set_lifecycle_hook([](std::string_view method, const Json&) {
        if (method == "set_cloud_enabled") {
            throw mw::EngineRefusal("AI insights are unavailable for archived playback");
        }
    });
    const auto refused = app.post_json("/api/meeting/cloud", kHostQuery, Json{{"enabled", false}});
    CHECK(refused.status == 409);
    CHECK(refused.body == R"({"detail":"AI insights are unavailable for archived playback"})");
}

TEST_CASE("the review route admits a host to review and skip a saved question", "[dashboard]") {
    auto passes = WebClient::default_passes();
    passes.review = [](std::shared_ptr<ms::MeetingStateStore> store) {
        return Json{{"ok", store->meeting_id() == "m_test"}, {"status", "ready"}};
    };
    auto state = fake_web_state();
    state.status = "ended";
    state.cloud_enabled = true;
    state.finalization.status = "completed";
    ms::CardItem item;
    item.id = "it_decision";
    item.card = "decisions";
    item.text = "The beta ships on Friday.";
    state.cards["decisions"].push_back(std::move(item));
    state.insight_review = Json{{"enabled", true},
                                {"consent", "typesafe-text-v1"},
                                {"status", "ready"},
                                {"questions", Json::array({Json{{"id", "rv_case"},
                                                                {"item_id", "it_decision"},
                                                                {"revision", 1},
                                                                {"field", "acceptance"},
                                                                {"evidence", Json::array()},
                                                                {"status", "open"}}})}};
    WebClient web(std::move(passes), std::nullopt, std::move(state));
    web.end_meeting();
    constexpr auto route = "/api/meetings/m_test/review";
    const auto start = web.app->post_json(route, kHostQuery, Json{{"op", "start"}});
    REQUIRE(start.status == 200);
    CHECK(start.json().at("ok") == true);
    CHECK(start.json().at("state").at("insight_review").at("status") == "ready");
    const Json skip{{"op", "review_skip"}, {"question_id", "rv_case"}};
    CHECK(web.app->post_json(route, kGuestQuery, skip).status == 403);
    const auto finished = web.app->post_json(route, kHostQuery, skip);
    REQUIRE(finished.status == 200);
    CHECK(finished.json().at("ok") == true);
    CHECK(finished.json().at("state").at("insight_review").at("questions")[0].at("status") ==
          "skipped");
}

TEST_CASE("host browser opt-in requests local approval and opt-out cancels it", "[dashboard]") {
    struct Request {
        std::string meeting_id;
        fv::CancelToken permit;
        mw::CloudConsentFinished finish;
    };
    std::promise<Request> pending;
    auto next = pending.get_future();
    WebClient web(WebClient::default_passes(), std::nullopt, fake_web_state(),
                  [&](std::string id, fv::CancelToken permit, mw::CloudConsentFinished finish) {
                      pending.set_value({std::move(id), std::move(permit), std::move(finish)});
                  });
    const std::string endpoint = "/api/meeting/cloud";
    const Json enable{{"enabled", true}};
    CHECK(web.app->post_json(endpoint, kGuestQuery, enable).status == 403);
    auto response = std::async(std::launch::async,
                               [&] { return web.app->post_json(endpoint, kHostQuery, enable); });
    REQUIRE(next.wait_for(3s) == std::future_status::ready);
    auto request = next.get();
    CHECK(request.meeting_id == "m_test");
    CHECK_FALSE(request.permit.cancelled());
    CHECK(response.wait_for(0ms) == std::future_status::timeout);
    CHECK(web.app->post_json(endpoint, kHostQuery, enable).status == 409);
    CHECK(web.app->post_json(endpoint, kHostQuery, Json{{"enabled", false}}).status == 200);
    CHECK(request.permit.cancelled());
    request.finish(std::nullopt);
    CHECK(response.get().status == 409);

    // The next host request needs a new local approval.
    pending = std::promise<Request>();
    next = pending.get_future();
    auto retry = std::async(std::launch::async,
                            [&] { return web.app->post_json(endpoint, kHostQuery, enable); });
    REQUIRE(next.wait_for(3s) == std::future_status::ready);
    request = next.get();
    CHECK_FALSE(request.permit.cancelled());
    request.finish(std::nullopt);
    const auto accepted = retry.get();
    CHECK(accepted.status == 200);
    CHECK(accepted.json().at("enabled") == true);
}

// from
// tests/test_meeting_web_auth.py::TestCloudConsent::test_enable_with_consent_and_disable_always_allowed
TEST_CASE("test_enable_with_consent_and_disable_always_allowed", "[test_meeting_web_auth]") {
    // Enabling waits for a fresh desktop approval (a30-dashboard-fresh-approval), whose desktop
    // side turns the engine on (MeetingRuntime::request_browser_insights); disabling needs none
    // and reaches the engine here. Both choices are remembered as the next meeting's default.
    WebClient web(WebClient::default_passes(), std::nullopt, fake_web_state(),
                  [](std::string, fv::CancelToken, mw::CloudConsentFinished finish) {
                      finish(std::nullopt);
                  });
    const std::string endpoint = "/api/meeting/cloud";
    web.app->settings.save_setting(fv::keys::meeting_cloud_last_enabled.key(), false);
    const auto enabled = web.app->post_json(endpoint, kHostQuery, Json{{"enabled", true}});
    CHECK(enabled.status == 200);
    CHECK(web.app->settings.get(fv::keys::meeting_cloud_last_enabled) == true);
    CHECK(web.engine->calls("set_cloud_enabled").empty());
    const auto disabled = web.app->post_json(endpoint, kHostQuery, Json{{"enabled", false}});
    CHECK(disabled.status == 200);
    CHECK(web.engine->calls("set_cloud_enabled").size() == 1);
    CHECK(web.app->settings.get(fv::keys::meeting_cloud_last_enabled) == false);

    // A declined approval remembers nothing.
    WebClient declined(WebClient::default_passes(), std::nullopt, fake_web_state(),
                       [](std::string, fv::CancelToken, mw::CloudConsentFinished finish) {
                           finish(std::string("AI insights were not approved on the desktop."));
                       });
    declined.app->settings.save_setting(fv::keys::meeting_cloud_last_enabled.key(), false);
    CHECK(declined.app->post_json(endpoint, kHostQuery, Json{{"enabled", true}}).status == 403);
    CHECK(declined.app->settings.get(fv::keys::meeting_cloud_last_enabled) == false);
}

TEST_CASE("a guest note request completes through the scheduler callback", "[dashboard]") {
    WebClient web;
    std::promise<std::function<void(Json)>> queued;
    auto completion = queued.get_future();
    web.engine->set_note_request([&](std::string, std::function<void(Json)> finish) {
        queued.set_value(std::move(finish));
    });
    auto response = std::async(std::launch::async, [&] {
        return web.app->post_json("/api/meeting/notes/request", kGuestQuery,
                                  Json{{"text", "Summarize the decision."}});
    });
    REQUIRE(completion.wait_for(3s) == std::future_status::ready);
    auto finish = completion.get();
    CHECK(response.wait_for(0ms) == std::future_status::timeout);
    CHECK(web.app->get("/api/search", {{"token", kHost}, {"q", "budget"}}).status == 200);
    finish(Json{{"ok", true}, {"applied", 1}, {"rejected", 0}, {"error", Json()}});
    const auto answer = response.get();
    CHECK(answer.status == 200);
    CHECK(answer.json().at("applied") == 1);
    const auto calls = web.engine->calls("request_note_adjustment");
    REQUIRE(calls.size() == 1);
    CHECK(calls.front().args[0] == "Summarize the decision.");
}

TEST_CASE("only a host semantic search reaches the configured meeting ranker", "[dashboard]") {
    auto passes = WebClient::default_passes();
    unsigned rankings = 0;
    passes.semantic_search = [&](std::string_view phrase) {
        ++rankings;
        return Json{{"results", Json::array()}, {"mode", "semantic"}, {"message", phrase}};
    };
    WebClient web(std::move(passes));
    CHECK(web.app->get("/api/search", {{"token", kGuest}, {"q", "release"}, {"mode", "semantic"}})
              .status == 403);
    CHECK(web.app->get("/api/search", {{"token", kHost}, {"q", "release"}, {"mode", "keyword"}})
              .status == 200);
    CHECK(rankings == 0);
    const auto result =
        web.app->get("/api/search", {{"token", kHost}, {"q", "release"}, {"mode", "semantic"}});
    CHECK(result.status == 200);
    CHECK(result.json().at("mode") == "semantic");
    CHECK(result.json().at("message") == "release");
    CHECK(rankings == 1);
}

TEST_CASE("semantic search falls back to keyword results with the settings' message",
          "[dashboard]") {
    WebClient web;
    auto& app = *web.app;
    const auto off =
        app.get("/api/search", {{"token", kHost}, {"q", " budget "}, {"mode", "semantic"}});
    REQUIRE(off.status == 200);
    CHECK(off.json()["mode"] == "keyword");
    CHECK(off.json()["message"] ==
          "Keyword results. Enable Semantic history search under Meeting settings → Fast "
          "judgments for meaning-based ranking.");
    CHECK(web.repo->search_calls() == std::vector<std::string>{"budget"});
    app.settings.save_setting(fv::keys::typesafe_enabled.key(), true);
    app.settings.save_setting(fv::keys::typesafe_semantic_search_enabled.key(), true);
    const auto on =
        app.get("/api/search", {{"token", kHost}, {"q", "budget"}, {"mode", "semantic"}});
    CHECK(on.json()["message"] == "Semantic ranking unavailable. Showing keyword results.");
}

TEST_CASE("the extensions take the intelligence routes", "[dashboard]") {
    class Recording final : public mw::DashboardExtensions {
    public:
        std::vector<std::string> calls;
        mw::DashboardResponse review(mw::DashboardApi&, const std::string& meeting_id,
                                     const Json& body) override {
            calls.push_back("review " + meeting_id + " " + body["op"].get<std::string>());
            return mw::DashboardResponse::json(Json{{"ok", true}});
        }
        mw::DashboardResponse request_report(mw::DashboardApi&, const std::string& meeting_id,
                                             const std::string& request) override {
            calls.push_back("report " + meeting_id + " " + request);
            throw mw::HttpError(409, "This meeting is holding the maximum number of reports.");
        }
        mw::DashboardResponse request_note(mw::DashboardApi&, const std::string& role,
                                           const std::string& text) override {
            calls.push_back("note " + role + " " + text);
            return mw::DashboardResponse::json(
                Json{{"ok", true}, {"applied", 1}, {"rejected", 0}, {"error", nullptr}});
        }
        mw::DashboardResponse set_cloud(mw::DashboardApi&, bool enabled) override {
            calls.push_back(enabled ? "cloud on" : "cloud off");
            return mw::DashboardResponse::json(Json{{"ok", true}, {"enabled", enabled}});
        }
        Json semantic_search(mw::DashboardApi&, std::string_view query) override {
            calls.push_back("search " + std::string(query));
            return Json{{"results", Json::array()}, {"mode", "semantic"}, {"message", "Ranked"}};
        }
    };
    WebClient web;
    auto extensions = std::make_shared<Recording>();
    web.app->api->set_extensions(extensions);
    auto& app = *web.app;
    CHECK(app.post_json("/api/meetings/m_test/review", kHostQuery, Json{{"op", "review_skip"}})
              .status == 200);
    const auto report =
        app.post_json("/api/meetings/m_test/reports", kHostQuery, Json{{"request", "a brief"}});
    CHECK(report.status == 409);
    CHECK(report.body == R"({"detail":"This meeting is holding the maximum number of reports."})");
    CHECK(app.post_json("/api/meeting/notes/request", kGuestQuery, Json{{"text", "Shorter"}})
              .status == 200);
    CHECK(app.post_json("/api/meeting/cloud", kHostQuery, Json{{"enabled", "false"}}).body ==
          R"({"ok":true,"enabled":true})");
    CHECK(app.get("/api/search", {{"token", kHost}, {"q", "x"}, {"mode", "semantic"}})
              .json()["mode"] == "semantic");
    CHECK(extensions->calls ==
          std::vector<std::string>{"review m_test review_skip", "report m_test a brief",
                                   "note guest Shorter", "cloud on", "search x"});
}

TEST_CASE("a handler failure answers 500 without the request path in the log", "[dashboard]") {
    WebClient web;
    web.engine->set_lifecycle_hook([](std::string_view method, const Json&) {
        if (method == "pause") {
            throw std::runtime_error("engine exploded");
        }
    });
    ScopedLogCapture logs;
    const auto r = web.app->post("/api/meeting/pause", kHostQuery);
    CHECK(r.status == 500);
    CHECK(r.body == "Internal Server Error");
    for (const auto& line : logs.capture->lines()) {
        CHECK_FALSE(contains(line, kHost));
        CHECK_FALSE(contains(line, kGuest));
    }
}

TEST_CASE("call routes a request without a socket", "[dashboard]") {
    WebClient web;
    auto& api = *web.app->api;
    CHECK(api.call("GET", "/api/session", {{"token", kHost}}).status == 200);
    const auto missing = api.call("GET", "/api/nowhere");
    CHECK(missing.status == 404);
    CHECK(missing.body == R"({"detail":"Not Found"})");
    const auto wrong = api.call("GET", "/api/meeting/end");
    CHECK(wrong.status == 405);
    CHECK(wrong.headers.front() == std::pair<std::string, std::string>{"allow", "POST"});
    const auto invalid = api.call("GET", "/api/transcript", {{"limit", "x"}});
    CHECK(invalid.status == 422);
}
