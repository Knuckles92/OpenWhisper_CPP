// The archive dashboard's insight review route: tests/test_meeting_insight_review.py's
// test_archive_api_host_action_survives_restart. The question is the one the review worker
// publishes (a system review_finish); FioriVoce serves the route only where a device-local judge
// exists (a30-dashboard-local-judgments), so the passes carry a review.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <string>

#include "api_harness.h"
#include "base/utf8.h"
#include "meeting_state/review_ops.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"
#include "meeting_web/archive.h"

using fv::Json;
using namespace fv::meeting_web::test_support;
namespace mw = fv::meeting_web;
namespace ms = fv::meeting_state;

namespace {

// make_store() and prepare(): an ended meeting with one action item and the worker's question.
Json reviewed_state(const fv::Clock& clock) {
    ms::MeetingState state;
    state.meeting_id = "synthetic";
    state.title = "Synthetic review";
    state.status = "ended";
    state.cloud_enabled = true;
    state.finalization.status = "completed";
    state.insight_review = Json{{"enabled", true},
                                {"consent", std::string(ms::REVIEW_CONSENT)},
                                {"sensitivity", "normal"},
                                {"status", "pending"},
                                {"questions", Json::array()},
                                {"message", ""}};
    ms::CardItem item;
    item.id = "it0";
    item.card = "action_items";
    item.text = "Send paper links 0";
    item.evidence = {"sg1"};
    item.created_at = item.updated_at = "2026-09-18T10:01:00Z";
    state.cards["action_items"].push_back(item);
    ms::MeetingStateStore store(std::move(state), nullptr, ms::SegmentHandler{}, clock);
    REQUIRE(store
                .apply("system", std::string("insight-review"),
                       Json::array({Json{{"op", "review_begin"}, {"run_id", "r"}}}))
                .at(0)
                .ok);
    const Json question{{"id", "rv_question"},
                        {"item_id", "it0"},
                        {"revision", 1},
                        {"field", "acceptance"},
                        {"text", "Was this agreed, or only proposed?"},
                        {"choices",
                         {{"accepted", "Agreed"},
                          {"offered", "Only proposed"},
                          {"edit", "Correct it"},
                          {"incorrect", "Incorrect"}}},
                        {"status", "open"},
                        {"evidence", Json::array({"sg1"})},
                        {"sources", Json::array()},
                        {"owners", Json::array()},
                        {"score", 0.3},
                        {"reason", "The evidence needs clarification on this point."},
                        {"priority", 1.4}};
    const Json assessment{{"item_id", "it0"},
                          {"revision", 1},
                          {"review",
                           {{"state", "provisional"},
                            {"scores", {{"acceptance", 0.3}}},
                            {"model", "jev-1.13.0"},
                            {"assessed_at", "2026-09-18T10:11:00Z"},
                            {"revision", 1}}}};
    REQUIRE(store
                .apply("system", std::string("insight-review"),
                       Json::array({Json{{"op", "review_finish"},
                                         {"run_id", "r"},
                                         {"status", "ready"},
                                         {"message", "Review the details below."},
                                         {"assessments", Json::array({assessment})},
                                         {"questions", Json::array({question})}}}))
                .at(0)
                .ok);
    const Json snapshot = store.snapshot();
    REQUIRE(snapshot.at("insight_review").at("questions").size() == 1);
    return snapshot;
}

} // namespace

// from tests/test_meeting_insight_review.py::test_archive_api_host_action_survives_restart
TEST_CASE("test_archive_api_host_action_survives_restart", "[test_meeting_insight_review]") {
    SqlWorld world;
    const Json state = reviewed_state(world.clock);
    world.repo->create_meeting({{"id", "synthetic"},
                                {"title", "Synthetic review"},
                                {"status", "ended"},
                                {"cloud_enabled", true},
                                {"host_token", "test-host"},
                                {"guest_token", "test-guest"},
                                {"spool_dir", fv::path_to_utf8(world.root.path())},
                                {"started_at", "2026-09-18T10:00:00Z"},
                                {"ended_at", "2026-09-18T10:10:00Z"},
                                {"state_json", state.dump()},
                                {"state_seq", state.at("seq")}});
    mw::PostMeetingPasses passes;
    // Checked at the end: the pass may run off the test's thread, where Catch cannot report.
    std::atomic<bool> review_started{false};
    passes.review = [&review_started](std::shared_ptr<ms::MeetingStateStore>) {
        review_started = true;
        return Json();
    };
    {
        mw::ArchivedMeetingDashboard archive(world.repo, *world.repo->get_meeting("synthetic"),
                                             world.root.path(), Json::object(), world.clock);
        ApiServer app(archive, world.repo, mw::sql_past_meeting_pages(world.repo), passes,
                      mw::DashboardOptions{}, world.root.path() / "settings.json", world.clock);
        archive.attach_server(app.server.get());
        const Json op{
            {"op", "review_answer"}, {"question_id", "rv_question"}, {"answer", "offered"}};
        const std::string route = "/api/meetings/synthetic/review";
        CHECK(app.post_json(route, {{"token", "test-guest"}}, op).status == 403);
        const auto response = app.post_json(route, {{"token", "test-host"}}, op);
        REQUIRE(response.status == 200);
        CHECK(response.json().at("ok") == true);
        archive.shutdown();
    }
    mw::ArchivedMeetingDashboard reopened(world.repo, *world.repo->get_meeting("synthetic"),
                                          world.root.path(), Json::object(), world.clock);
    const Json snapshot = reopened.store()->snapshot();
    CHECK(snapshot.at("insight_review").at("questions").at(0).at("status") == "answered");
    CHECK(snapshot.at("cards").at("action_items").at(0).at("data").at("commitment") == "offered");
    CHECK_FALSE(review_started.load());
}
