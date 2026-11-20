// Audit A29-01: the report route's refusals in plain words (meeting/web/api.py
// _REPORT_REJECTIONS) for a meeting that is not ready and one holding MAX_CUSTOM_REPORTS, over the
// store's real request_custom_report claim.

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <memory>
#include <string>

#include "api_harness.h"
#include "fake_web_repo.h"
#include "meeting_state/custom_report_ops.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"
#include "meeting_web/testing/fake_dashboard_engine.h"

using fv::Json;
using namespace fv::meeting_web::test_support;
namespace mw = fv::meeting_web;
namespace ms = fv::meeting_state;

namespace {

const std::string kUrl = "/api/meetings/m_past/reports";

Json past_meeting(const ms::MeetingState& state) {
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
                {"state_json", state.to_json().dump()}};
}

// The report pass claims through the store as start_custom_report does and returns its reason.
struct ReportApi {
    explicit ReportApi(const ms::MeetingState& past) {
        store = std::make_shared<ms::MeetingStateStore>(fake_web_state(), nullptr,
                                                        ms::SegmentHandler{}, clock, ids);
        engine = std::make_unique<mw::testing::FakeDashboardEngine>("m_test", store,
                                                                    root.path() / "meetings");
        repo->set_past(past_meeting(past));
        mw::PostMeetingPasses passes;
        passes.report = [](std::shared_ptr<ms::MeetingStateStore> owner, const Json&,
                           std::string_view request, fv::Executor&, const fv::CancelToken&) {
            const auto claimed = owner->apply("host", std::nullopt,
                                              Json::array({Json{{"op", "request_custom_report"},
                                                                {"request", request},
                                                                {"run_id", "run_api"}}}));
            if (claimed.empty() || !claimed.front().ok) {
                return Json{{"ok", false},
                            {"report_id", nullptr},
                            {"error", claimed.empty() ? Json("rejected")
                                                      : Json(claimed.front().reason.value_or(""))}};
            }
            return Json{{"ok", true}, {"report_id", claimed.front().target_id.value_or("")}};
        };
        mw::DashboardOptions options;
        options.dist_root = root.path() / "no-bundle";
        options.meetings_root = root.path() / "meetings";
        app = std::make_unique<ApiServer>(*engine, repo, repo, std::move(passes),
                                          std::move(options), root.path() / "settings.json", clock);
    }

    fv::testkit::TempDataRoot root;
    fv::FakeClock clock{fixture_now()};
    ms::testing::FixedIds ids;
    std::shared_ptr<FakeWebRepo> repo = std::make_shared<FakeWebRepo>();
    std::shared_ptr<ms::MeetingStateStore> store;
    std::unique_ptr<mw::testing::FakeDashboardEngine> engine;
    std::unique_ptr<ApiServer> app;
};

ms::MeetingState ended(const std::string& finalization) {
    ms::MeetingState state;
    state.meeting_id = "m_past";
    state.title = "Vendor renewal";
    state.status = "ended";
    state.cloud_enabled = true;
    state.finalization =
        ms::FinalizationState::coerce(Json{{"status", finalization}}, true, "ended");
    return state;
}

std::string refusal(const ms::MeetingState& state, const std::string& url = kUrl) {
    ReportApi api(state);
    const auto response = api.app->post_json(url, {{"token", std::string(kFakeHostToken)}},
                                             Json{{"request", "every commitment"}});
    REQUIRE(response.status == 409);
    return response.json().at("detail").get<std::string>();
}

} // namespace

TEST_CASE("audit A29-01: a report waits for the meeting's insights to finish",
          "[test_meeting_custom_report]") {
    // The live meeting m_test is still active; a saved one is settled when it is opened.
    CHECK(refusal(ended("completed"), "/api/meetings/m_test/reports") ==
          "Reports are written once the meeting has ended and its insights have finished.");
}

TEST_CASE("audit A29-01: a meeting holds at most 20 reports", "[test_meeting_custom_report]") {
    auto state = ended("completed");
    for (std::size_t i = 0; i < ms::MAX_CUSTOM_REPORTS; ++i) {
        ms::CustomReport report;
        report.id = std::format("rep_{}", i);
        report.request = "x";
        report.status = "ready";
        report.run_id = std::format("run_{}", i);
        report.created_at = report.updated_at = "2026-09-19T11:00:00Z";
        state.custom_reports.push_back(report);
    }
    CHECK(refusal(state) ==
          "This meeting is holding the maximum number of reports. Delete one to make room.");
}
