// Checklist line MR-01 (docs/port/map/a32-user-facing-features.md): insight review's "Normal vs
// Thorough sensitivity (thresholds 0.85/0.95)". insight_review.py _review_state: a score under the
// meeting's threshold is a concern, so a decision scored 0.9 everywhere asks nothing at Normal and
// asks about it at Thorough.

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../meeting_state/stub_repository.h"
#include "base/testing/fake_clock.h"
#include "llm/testing/fake_judge.h"
#include "meeting_intel/insight_review.h"
#include "meeting_state/review_ops.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"
#include "meeting_state/testing/fixed_ids.h"

using fv::Json;
using namespace fv::meeting_state;

namespace {

class Repo final : public test_support::StubRepository {
public:
    std::vector<Json> get_segments(std::string_view, double, std::optional<std::int64_t>) override {
        return {Json{{"id", "sg1"}, {"start_s", 4}, {"text", "We will ship the beta on Friday."}}};
    }
    Json on_ops_applied(std::string_view, const Json&, std::vector<OpResult>&, std::string_view,
                        const std::optional<std::string>&) override {
        return Json();
    }
    Json persist_state(std::string_view, const Json&) override { return Json(); }
};

// One decision, every asked score `score` (the inverted checks report no concern).
Json review_at(const std::string& sensitivity, double score) {
    fv::FakeClock clock;
    testing::FixedIds ids;
    auto repo = std::make_shared<Repo>();
    MeetingState state;
    state.meeting_id = "synthetic";
    state.status = "ended";
    state.cloud_enabled = true;
    state.finalization.status = "completed";
    state.insight_review = Json{{"enabled", true},
                                {"consent", std::string(REVIEW_CONSENT)},
                                {"sensitivity", sensitivity},
                                {"status", "pending"},
                                {"questions", Json::array()},
                                {"message", ""}};
    CardItem decision;
    decision.id = "d1";
    decision.card = "decisions";
    decision.text = "Ship the beta on Friday";
    decision.evidence = {"sg1"};
    decision.created_at = decision.updated_at = "2026-01-01T00:00:00Z";
    state.cards["decisions"].push_back(decision);
    MeetingStateStore store(state, repo, SegmentHandler{}, clock, ids);

    auto judge = std::make_shared<fv::llm::FakeJudge>();
    judge->set_local(true);
    judge->set_model("local-judge");
    judge->set_handler([score](const Json&, const Json& questions) {
        Json answers = Json::object();
        for (auto it = questions.begin(); it != questions.end(); ++it) {
            const bool inverted = it.key() == "contradiction" || it.key() == "asr_uncertain";
            answers[it.key()] = Json{{"noul", inverted ? 0.0 : score}};
        }
        return std::optional(answers);
    });
    fv::meeting_intel::run_insight_review(store, *repo,
                                          {.judge = [judge](double) { return judge; },
                                           .allowed = [] { return true; },
                                           .clock = &clock});
    return store.snapshot().at("insight_review");
}

} // namespace

TEST_CASE("a32 MR-01: Thorough asks below 0.95 where Normal asks only below 0.85", "[audit_a32]") {
    const Json normal = review_at("normal", 0.9);
    CHECK(normal.at("status") == "ready");
    CHECK(normal.at("questions").empty());

    const Json thorough = review_at("thorough", 0.9);
    CHECK(thorough.at("status") == "ready");
    REQUIRE_FALSE(thorough.at("questions").empty());
    CHECK(thorough.at("questions")[0].at("item_id") == "d1");

    // Both thresholds are exclusive: a score at the threshold is no concern.
    CHECK(review_at("normal", 0.85).at("questions").empty());
    CHECK(review_at("thorough", 0.95).at("questions").empty());
    CHECK_FALSE(review_at("normal", 0.84).at("questions").empty());
}
