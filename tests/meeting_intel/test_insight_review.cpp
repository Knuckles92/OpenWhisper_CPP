#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "llm/testing/fake_judge.h"
#include "meeting_intel/insight_review.h"
#include "meeting_state/testing/fake_meeting_repository.h"

namespace {

using fv::Json;

struct ReviewFixture {
    fv::meeting_state::testing::FakeMeetingRepository repo;
    std::shared_ptr<fv::meeting_state::MeetingStateStore> store;
    std::shared_ptr<fv::llm::FakeJudge> judge = std::make_shared<fv::llm::FakeJudge>();

    explicit ReviewFixture(bool consent = true) {
        repo.create_meeting(Json{{"id", "m_review"},
                                 {"status", "ended"},
                                 {"started_at", "2026-11-17T09:00:00+00:00"},
                                 {"host_token", "host"},
                                 {"guest_token", "guest"},
                                 {"spool_dir", "spool"}});
        add_segment("sg_claim", 10, "We can ship Friday if the bugs are fixed.");
        add_segment("sg_later", 18, "Friday is no longer possible for the beta.");
        fv::meeting_state::MeetingState state;
        state.meeting_id = "m_review";
        state.cloud_enabled = true;
        state.insight_review = Json{{"enabled", true},
                                    {"status", "pending"},
                                    {"consent", consent ? "typesafe-text-v1" : ""},
                                    {"sensitivity", "normal"},
                                    {"questions", Json::array()}};
        store = std::make_shared<fv::meeting_state::MeetingStateStore>(std::move(state));
        const auto added =
            store->apply("agent", std::string("notes"),
                         Json::array({Json{{"op", "add_item"},
                                           {"card", "decisions"},
                                           {"text", "The beta ships Friday."},
                                           {"evidence", Json::array({"sg_claim"})}}}));
        REQUIRE(added.size() == 1);
        REQUIRE(added.front().ok);
        REQUIRE(store->update_runtime_fields(
            Json{{"status", "ended"}, {"finalization", {{"status", "completed"}}}}));
        judge->set_local(true);
        judge->set_model("fixture-judge");
    }

    void add_segment(std::string id, double start, std::string text) {
        fv::meeting_state::TranscriptSegment row;
        row.segment_id = std::move(id);
        row.meeting_id = "m_review";
        row.channel = "mic";
        row.start_s = start;
        row.end_s = start + 2;
        row.text = std::move(text);
        repo.add_segments({row});
    }

    Json run() {
        return fv::meeting_intel::run_insight_review(*store, repo,
                                                     {.judge = [this](double) { return judge; },
                                                      .allowed =
                                                          [] {
                                                              return true;
                                                          }});
    }

    static Json contradictory() {
        return {{"support", {{"noul", 0.95}}},
                {"acceptance", {{"noul", 0.12}}},
                {"contradiction", {{"noul", 0.99}}},
                {"asr_uncertain", {{"noul", 0.01}}}};
    }
};

TEST_CASE("local insight review asks about later contrary speech without rewriting the claim",
          "[insight_review]") {
    ReviewFixture fixture;
    fixture.judge->push(ReviewFixture::contradictory());
    const auto result = fixture.run();
    REQUIRE(result.at("ok") == true);
    REQUIRE(result.at("status") == "ready");
    const auto state = fixture.store->snapshot();
    const auto& claim = state.at("cards").at("decisions")[0];
    CHECK(claim.at("text") == "The beta ships Friday.");
    CHECK(claim.at("revision") == 1);
    CHECK(claim.at("review").at("state") == "unsupported");
    const auto& questions = state.at("insight_review").at("questions");
    REQUIRE(questions.size() == 1);
    CHECK(questions[0].at("field") == "acceptance");
    CHECK(questions[0].at("sources")[1].at("id") == "sg_later");
    const auto calls = fixture.judge->calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].state.at("transcript")[1].at("text") ==
          "Friday is no longer possible for the beta.");
}

TEST_CASE("insight review never submits a meeting to a non-local judge", "[insight_review]") {
    ReviewFixture fixture;
    fixture.judge->set_local(false);
    CHECK(fixture.run().at("status") == "unavailable");
    CHECK(fixture.judge->calls().empty());
    CHECK(fixture.store->snapshot().at("insight_review").at("questions").empty());
}

TEST_CASE("an insight review whose evidence changed discards the judgments", "[insight_review]") {
    ReviewFixture fixture;
    fixture.judge->set_handler([&](const Json&, const Json&) {
        fixture.add_segment("sg_changed", 31, "A late transcript change.");
        return std::optional(ReviewFixture::contradictory());
    });
    const auto result = fixture.run();
    CHECK(result.at("status") == "unavailable");
    const auto review = fixture.store->snapshot().at("insight_review");
    CHECK(review.at("questions").empty());
    CHECK(review.at("message") ==
          "The transcript changed during review. Retry to check the latest version.");
}

TEST_CASE("insight review requires its own recorded opt-in", "[insight_review]") {
    ReviewFixture fixture(false);
    const auto result = fixture.run();
    CHECK(result.at("ok") == false);
    CHECK(result.at("error") == "review_disabled");
    CHECK(fixture.judge->calls().empty());
}

} // namespace
