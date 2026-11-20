// Behaviour of the a30 area map (docs/port/map/a30-meeting-web-server-and-dashboard-contract.md)
// that the ported tests and the recorded transcript leave unpinned: the page limits the routes
// pass to the repository (A30-08, A30-09, A30-21) and the exact 409 wording of the single-writer
// guards (A30-16, A30-18, A30-19, A30-38).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "api_harness.h"
#include "fake_web_repo.h"
#include "meeting_state/store.h"
#include "meeting_web/testing/fake_dashboard_engine.h"

using fv::Json;
using namespace fv::meeting_web::test_support;
namespace mw = fv::meeting_web;
namespace ms = fv::meeting_state;
using namespace std::chrono_literals;

namespace {

const std::string kHost(kFakeHostToken);

// FakeWebRepo that records the page sizes the routes ask for.
class LimitRecordingRepo final : public FakeWebRepo {
public:
    std::vector<Json> get_segments_page(std::string_view meeting_id,
                                        std::optional<double> cursor_start_s,
                                        const std::optional<std::string>& cursor_id,
                                        std::int64_t limit) override {
        record(segment_limits_, limit);
        return FakeWebRepo::get_segments_page(meeting_id, cursor_start_s, cursor_id, limit);
    }
    std::vector<Json> list_past_meeting_summaries(const ms::PastMeetingQuery& query) override {
        record(history_limits_, query.limit);
        return FakeWebRepo::list_past_meeting_summaries(query);
    }
    std::vector<Json> list_events(std::string_view meeting_id, std::optional<std::int64_t> before,
                                  std::int64_t limit) override {
        record(event_limits_, limit);
        return FakeWebRepo::list_events(meeting_id, before, limit);
    }

    std::int64_t last_segment_limit() const { return last(segment_limits_); }
    std::int64_t last_history_limit() const { return last(history_limits_); }
    std::int64_t last_event_limit() const { return last(event_limits_); }

private:
    void record(std::vector<std::int64_t>& into, std::int64_t limit) {
        std::lock_guard lock(limits_mutex_);
        into.push_back(limit);
    }
    std::int64_t last(const std::vector<std::int64_t>& from) const {
        std::lock_guard lock(limits_mutex_);
        REQUIRE_FALSE(from.empty());
        return from.back();
    }

    mutable std::mutex limits_mutex_;
    std::vector<std::int64_t> segment_limits_;
    std::vector<std::int64_t> history_limits_;
    std::vector<std::int64_t> event_limits_;
};

struct Client {
    explicit Client(mw::PostMeetingPasses passes, ms::MeetingState state = fake_web_state()) {
        store = std::make_shared<ms::MeetingStateStore>(std::move(state), nullptr,
                                                        ms::SegmentHandler{}, clock, ids);
        engine = std::make_unique<mw::testing::FakeDashboardEngine>("m_test", store,
                                                                    root.path() / "meetings");
        mw::DashboardOptions options;
        options.dist_root = root.path() / "no-bundle";
        options.meetings_root = root.path() / "meetings";
        app = std::make_unique<ApiServer>(*engine, repo, repo, std::move(passes),
                                          std::move(options), root.path() / "settings.json", clock);
    }

    fv::testkit::TempDataRoot root;
    fv::FakeClock clock{fixture_now()};
    ms::testing::FixedIds ids;
    std::shared_ptr<LimitRecordingRepo> repo = std::make_shared<LimitRecordingRepo>();
    std::shared_ptr<ms::MeetingStateStore> store;
    std::unique_ptr<mw::testing::FakeDashboardEngine> engine;
    std::unique_ptr<ApiServer> app;
};

mw::PostMeetingPasses idle_passes() {
    mw::PostMeetingPasses passes;
    passes.rerun_insights = [](const std::string& meeting_id, const std::string&,
                               const fv::CancelToken&) {
        return Json{{"ok", true},
                    {"state", Json{{"meeting_id", meeting_id}, {"seq", 1}}},
                    {"applied", 0},
                    {"error", nullptr}};
    };
    passes.rerun_speakers = [](const std::string&, std::shared_ptr<ms::MeetingStateStore>) {
        return Json{{"ok", true}};
    };
    passes.review = [](std::shared_ptr<ms::MeetingStateStore>) {
        return Json{{"ok", true}};
    };
    return passes;
}

std::string detail_of(const HttpResult& result) {
    return result.json()["detail"].get<std::string>();
}

} // namespace

// A30-08 (limit clamped to 1..1000, limit+1 fetched), A30-09 (1..100, limit+1 fetched) and the
// a30 map's "Events paging": limit (default 100) reaches the repository unclamped.
TEST_CASE("a30: the routes ask the repository for the page sizes OpenWhisper asks for",
          "[audit_a30]") {
    Client web(idle_passes());
    auto& app = *web.app;
    const Query host{{"token", kHost}};
    const auto with = [&host](std::string name, std::string value) {
        Query query = host;
        query.emplace_back(std::move(name), std::move(value));
        return query;
    };

    REQUIRE(app.get("/api/transcript", host).status == 200);
    CHECK(web.repo->last_segment_limit() == 501);
    REQUIRE(app.get("/api/transcript", with("limit", "5000")).status == 200);
    CHECK(web.repo->last_segment_limit() == 1001);
    REQUIRE(app.get("/api/transcript", with("limit", "-3")).status == 200);
    CHECK(web.repo->last_segment_limit() == 2);
    REQUIRE(app.get("/api/meetings/m_test/transcript", with("limit", "1000")).status == 200);
    CHECK(web.repo->last_segment_limit() == 1001);

    REQUIRE(app.get("/api/meetings", host).status == 200);
    CHECK(web.repo->last_history_limit() == 51);
    REQUIRE(app.get("/api/meetings", with("limit", "500")).status == 200);
    CHECK(web.repo->last_history_limit() == 101);
    REQUIRE(app.get("/api/meetings", with("limit", "0")).status == 200);
    CHECK(web.repo->last_history_limit() == 2);

    REQUIRE(app.get("/api/events", host).status == 200);
    CHECK(web.repo->last_event_limit() == 100);
    REQUIRE(app.get("/api/events", with("limit", "5000")).status == 200);
    CHECK(web.repo->last_event_limit() == 5000);
}

// A30-16 (review while a pass holds the meeting), A30-18 (a second speaker pass), A30-19 (delete
// while a pass runs) and A30-38 (the claim is released afterwards).
TEST_CASE("a30: a claimed meeting refuses review, speakers and delete in OpenWhisper's words",
          "[audit_a30]") {
    std::promise<void> running;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    mw::PostMeetingPasses passes = idle_passes();
    passes.rerun_insights = [&running, released](const std::string& meeting_id, const std::string&,
                                                 const fv::CancelToken&) {
        running.set_value();
        released.wait_for(10s);
        return Json{{"ok", true},
                    {"state", Json{{"meeting_id", meeting_id}, {"seq", 1}}},
                    {"applied", 0},
                    {"error", nullptr}};
    };
    Client web(std::move(passes));
    web.engine->set_active(false);
    auto& app = *web.app;
    const Query host{{"token", kHost}};

    std::optional<HttpResult> first;
    std::thread worker([&] { first = app.post("/api/meetings/m_test/reinsights", host); });
    REQUIRE(running.get_future().wait_for(10s) == std::future_status::ready);

    const auto review =
        app.post_json("/api/meetings/m_test/review", host, Json{{"op", "review_skip"}});
    CHECK(review.status == 409);
    CHECK(detail_of(review) == "Wait for final insights to finish");
    const auto speakers = app.post("/api/meetings/m_test/respeakers", host);
    CHECK(speakers.status == 409);
    CHECK(detail_of(speakers) == "a post-meeting pass is already running for this meeting");
    const auto removed = app.del("/api/meetings/m_test", host);
    CHECK(removed.status == 409);
    CHECK(detail_of(removed) == "Wait for the meeting update to finish before deleting");

    release.set_value();
    worker.join();
    REQUIRE(first.has_value());
    CHECK(first->status == 200);
    // The claim is released with the answer.
    CHECK(app.post_json("/api/meetings/m_test/review", host, Json{{"op", "review_skip"}}).status ==
          200);
}

// A30-38 _refuse_rerun_while_busy: a running insight review and a report being written refuse a
// re-run and a delete with their own sentence.
TEST_CASE("a30: work in flight in the meeting's state refuses re-runs and delete", "[audit_a30]") {
    SECTION("insight review") {
        ms::MeetingState state = fake_web_state();
        state.insight_review = Json{{"status", "running"}};
        Client web(idle_passes(), std::move(state));
        web.engine->set_active(false);
        const Query host{{"token", kHost}};
        const auto rerun = web.app->post("/api/meetings/m_test/reinsights", host);
        CHECK(rerun.status == 409);
        CHECK(detail_of(rerun) == "Wait for insight review to finish");
        // Delete checks the review first, with its own wording.
        const auto removed = web.app->del("/api/meetings/m_test", host);
        CHECK(removed.status == 409);
        CHECK(detail_of(removed) == "Wait for the meeting update to finish before deleting");
    }
    SECTION("a report being written") {
        ms::MeetingState state = fake_web_state();
        ms::CustomReport report;
        report.id = "r_1";
        report.request = "Brief";
        report.status = "running";
        state.custom_reports.push_back(report);
        Client web(idle_passes(), std::move(state));
        web.engine->set_active(false);
        const Query host{{"token", kHost}};
        const auto rerun = web.app->post("/api/meetings/m_test/respeakers", host);
        CHECK(rerun.status == 409);
        CHECK(detail_of(rerun) == "Wait for the report being written to finish");
        const auto removed = web.app->del("/api/meetings/m_test", host);
        CHECK(removed.status == 409);
        CHECK(detail_of(removed) == "Wait for the report being written to finish");
    }
}
