// The report request route over the post-meeting report pass (DashboardApi::request_report with
// PostMeetingPasses::report): tests/test_meeting_custom_report.py TestApi. The pass stands in for
// start_custom_report and records what it was started with; in the app it is
// MeetingRuntime::post_meeting_passes' report, which asks the desktop first
// (a30-dashboard-fresh-approval).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "api_harness.h"
#include "fake_web_repo.h"
#include "meeting_state/custom_report_ops.h"
#include "meeting_state/store.h"
#include "meeting_web/testing/fake_dashboard_engine.h"

using fv::Json;
using namespace fv::meeting_web::test_support;
namespace mw = fv::meeting_web;
namespace ms = fv::meeting_state;

namespace {

const std::string kHost(kFakeHostToken);
const std::string kGuest(kFakeGuestToken);
const std::string kUrl = "/api/meetings/m_past/reports";

ms::MeetingState ended_state() {
    ms::MeetingState state;
    state.meeting_id = "m_past";
    state.title = "Vendor renewal";
    state.status = "ended";
    state.cloud_enabled = true;
    state.finalization =
        ms::FinalizationState::coerce(Json{{"status", "completed"}}, true, "ended");
    return state;
}

Json past_meeting(const Json& state = ended_state().to_json()) {
    return Json{{"id", "m_past"},
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
                {"state_json", state.dump()}};
}

// The api_client fixture: the live meeting m_test beside the ended m_past, and a report pass
// that claims a slot and publishes "# Brief" at once.
struct ReportApi {
    explicit ReportApi(Json past = past_meeting()) {
        store = std::make_shared<ms::MeetingStateStore>(fake_web_state(), nullptr,
                                                        ms::SegmentHandler{}, clock, ids);
        engine = std::make_unique<mw::testing::FakeDashboardEngine>("m_test", store,
                                                                    root.path() / "meetings");
        repo->set_past(std::move(past));
        mw::PostMeetingPasses passes;
        passes.report = [this](std::shared_ptr<ms::MeetingStateStore> owner, const Json& meeting,
                               std::string_view request, fv::Executor&, const fv::CancelToken&) {
            {
                const std::lock_guard lock(mutex);
                started.push_back(meeting);
            }
            const auto claimed = owner->apply("host", std::nullopt,
                                              Json::array({Json{{"op", "request_custom_report"},
                                                                {"request", request},
                                                                {"run_id", "run_api"}}}));
            if (claimed.empty() || !claimed.front().ok) {
                return Json{{"ok", false},
                            {"error", claimed.empty() ? Json("report_running")
                                                      : Json(claimed.front().reason.value_or(""))}};
            }
            const auto id = claimed.front().target_id.value_or("");
            if (!leave_running) {
                owner->apply("system", std::string(ms::WORKER_ACTOR),
                             Json::array({Json{{"op", "finish_custom_report"},
                                               {"report_id", id},
                                               {"run_id", "run_api"},
                                               {"status", "ready"},
                                               {"markdown", "# Brief\n\nEvery commitment."},
                                               {"title", "Brief"}}}));
            }
            return Json{{"ok", true}, {"report_id", id}, {"error", nullptr}};
        };
        mw::DashboardOptions options;
        options.dist_root = root.path() / "no-bundle";
        options.meetings_root = root.path() / "meetings";
        app = std::make_unique<ApiServer>(*engine, repo, repo, std::move(passes),
                                          std::move(options), root.path() / "settings.json", clock);
    }

    std::size_t starts() {
        const std::lock_guard lock(mutex);
        return started.size();
    }

    fv::testkit::TempDataRoot root;
    fv::FakeClock clock{fixture_now()};
    ms::testing::FixedIds ids;
    std::shared_ptr<FakeWebRepo> repo = std::make_shared<FakeWebRepo>();
    std::shared_ptr<ms::MeetingStateStore> store;
    std::unique_ptr<mw::testing::FakeDashboardEngine> engine;
    std::mutex mutex;
    std::vector<Json> started;
    bool leave_running = false;
    std::unique_ptr<ApiServer> app;
};

} // namespace

// from
// tests/test_meeting_custom_report.py::TestApi::test_a_host_request_writes_a_report_and_returns_the_state
TEST_CASE("test_a_host_request_writes_a_report_and_returns_the_state",
          "[test_meeting_custom_report]") {
    ReportApi api;
    const auto response =
        api.app->post_json(kUrl, {{"token", kHost}}, Json{{"request", "every commitment"}});
    REQUIRE(response.status == 200);
    const Json body = response.json();
    CHECK(body.at("ok") == true);
    CHECK_FALSE(body.at("report_id").get<std::string>().empty());
    const Json report = body.at("state").at("custom_reports").at(0);
    CHECK(report.at("status") == "ready");
    CHECK(report.at("markdown").get<std::string>().starts_with("# Brief"));
    // The meeting's own recorded endpoint is what the report runs against.
    REQUIRE(api.starts() == 1);
    CHECK(api.started[0].at("agent_provider") == "openrouter");
    CHECK(api.started[0].at("agent_model") == "test-model");
}

// from tests/test_meeting_custom_report.py::TestApi::test_only_the_host_may_ask
TEST_CASE("test_only_the_host_may_ask", "[test_meeting_custom_report]") {
    ReportApi api;
    CHECK(api.app->post_json(kUrl, {}, Json{{"request", "x"}}).status == 401);
    CHECK(api.app->post_json(kUrl, {{"token", kGuest}}, Json{{"request", "x"}}).status == 403);
    CHECK(api.starts() == 0);
}

// from tests/test_meeting_custom_report.py::TestApi::test_bad_requests_are_refused_before_any_work
TEST_CASE("test_bad_requests_are_refused_before_any_work", "[test_meeting_custom_report]") {
    const Json payload =
        GENERATE(Json{{"request", ""}}, Json{{"request", "   "}}, Json{{"request", 42}},
                 Json{{"request", std::string(ms::MAX_REQUEST_CHARS + 1, 'x')}}, Json::object());
    ReportApi api;
    CHECK(api.app->post_json(kUrl, {{"token", kHost}}, payload).status == 400);
    CHECK(api.starts() == 0);
}

// from tests/test_meeting_custom_report.py::TestApi::test_an_unknown_meeting_is_a_404
TEST_CASE("test_an_unknown_meeting_is_a_404", "[test_meeting_custom_report]") {
    ReportApi api;
    CHECK(
        api.app
            ->post_json("/api/meetings/m_nope/reports", {{"token", kHost}}, Json{{"request", "x"}})
            .status == 404);
    CHECK(api.starts() == 0);
}

// from
// tests/test_meeting_custom_report.py::TestApi::test_a_second_request_while_one_runs_is_refused_in_plain_words
TEST_CASE("test_a_second_request_while_one_runs_is_refused_in_plain_words",
          "[test_meeting_custom_report]") {
    SECTION("a stored running report was interrupted, so the slot is free") {
        Json state = ended_state().to_json();
        state["custom_reports"] = Json::array({Json{{"id", "rep_live"},
                                                    {"request", "x"},
                                                    {"status", "running"},
                                                    {"run_id", "run_1"},
                                                    {"markdown", ""},
                                                    {"title", ""},
                                                    {"message", ""},
                                                    {"sources", Json::object()},
                                                    {"requested_by", nullptr},
                                                    {"created_at", "2026-09-19T11:00:00Z"},
                                                    {"updated_at", "2026-09-19T11:00:00Z"}}});
        ReportApi api(past_meeting(state));
        const auto response =
            api.app->post_json(kUrl, {{"token", kHost}}, Json{{"request", "another"}});
        CHECK(response.status == 200);
    }
    SECTION("a genuinely concurrent request is refused") {
        ReportApi api;
        api.leave_running = true;
        REQUIRE(api.app->post_json(kUrl, {{"token", kHost}}, Json{{"request", "first"}}).status ==
                200);
        const auto response =
            api.app->post_json(kUrl, {{"token", kHost}}, Json{{"request", "another"}});
        CHECK(response.status == 409);
        CHECK(response.json().at("detail").get<std::string>().find("already being written") !=
              std::string::npos);
    }
}
