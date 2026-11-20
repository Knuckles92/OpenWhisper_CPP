// tests/test_meeting_insight_review.py: the review ops' gates in the store. The ops over the
// questions the review worker publishes run against the real worker (meeting_intel's
// run_insight_review) in tests/meeting_intel/test_review_worker.cpp.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "base/testing/fake_clock.h"
#include "meeting_state/export_markdown.h"
#include "meeting_state/review_ops.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"
#include "meeting_state/testing/fixed_ids.h"
#include "settings/enums.h"
#include "stub_repository.h"

using fv::Json;
using namespace fv::meeting_state;
using fv::meeting_state::test_support::StubRepository;

namespace {

// The Repo of the Python test: keeps what was saved, and fails on demand.
class Repo final : public StubRepository {
public:
    std::vector<Json> segments{Json{{"id", "sg1"},
                                    {"text", "I could send the paper links."},
                                    {"start_s", 10},
                                    {"speaker_participant_id", "p1"}}};
    Json saved;
    bool fail = false;

    Json on_ops_applied(std::string_view, const Json& state, std::vector<OpResult>&,
                        std::string_view, const std::optional<std::string>&) override {
        if (fail) {
            throw std::runtime_error("test persistence failure");
        }
        saved = state;
        return Json();
    }
    Json persist_state(std::string_view, const Json& state) override {
        saved = state;
        return Json();
    }
};

// review_config(enabled=True, consent=CONSENT).
Json review_config(bool enabled, const std::string& consent, bool cloud_enabled) {
    const bool on = enabled && consent == REVIEW_CONSENT;
    const std::string status = !on ? "disabled" : (cloud_enabled ? "pending" : "unavailable");
    return Json{{"enabled", on},
                {"consent", consent},
                {"sensitivity", "normal"},
                {"status", status},
                {"questions", Json::array()},
                {"message", cloud_enabled ? "" : "AI insights are off for this meeting."}};
}

struct Fixture {
    fv::FakeClock clock;
    testing::FixedIds ids;
    std::shared_ptr<Repo> repo = std::make_shared<Repo>();
    std::unique_ptr<MeetingStateStore> store;

    explicit Fixture(int count = 1, std::string consent = std::string(REVIEW_CONSENT),
                     bool enabled = true, bool cloud = true) {
        MeetingState state;
        state.meeting_id = "synthetic";
        state.status = "ended";
        state.cloud_enabled = cloud;
        state.finalization.status = "completed";
        state.insight_review = review_config(enabled, consent, cloud);
        for (int i = 0; i < count; ++i) {
            CardItem item;
            item.id = std::format("it{}", i);
            item.card = "action_items";
            item.text = std::format("Send paper links {}", i);
            item.evidence = {"sg1"};
            item.created_at = item.updated_at = "2026-01-01T00:00:00Z";
            state.cards["action_items"].push_back(item);
        }
        CardItem note;
        note.id = "note1";
        note.card = "live_notes";
        note.text = "Paper links will be sent.";
        note.evidence = {"sg1"};
        note.created_at = note.updated_at = "2026-01-01T00:00:00Z";
        state.cards["live_notes"].push_back(note);
        store = std::make_unique<MeetingStateStore>(state, repo, SegmentHandler{}, clock, ids);
    }
};

} // namespace

// from
// tests/test_meeting_insight_review.py::test_settings_and_meeting_state_agree_on_the_consent_version
TEST_CASE("test_settings_and_meeting_state_agree_on_the_consent_version",
          "[test_meeting_insight_review]") {
    CHECK(fv::enums::kInsightReviewConsent == REVIEW_CONSENT);
    CHECK(REVIEW_CONSENT == "typesafe-text-v1");
}

TEST_CASE("review ops: the worker's gates and an unavailable review", "[meeting_state_review]") {
    SECTION("no consent, no review") {
        Fixture f(1, "");
        const auto begun = f.store->apply(
            "system", std::nullopt, Json::parse(R"([{"op": "review_begin", "run_id": "r"}])"));
        CHECK(begun[0].reason == "review_disabled");
        CHECK(
            f.store
                ->apply("system", std::nullopt, Json::parse(R"([{"op": "review_unavailable"}])"))[0]
                .ok);
        CHECK(f.store->snapshot()["insight_review"]["status"] == "unavailable");
    }
    SECTION("a running review, a stale run and a missing run id") {
        Fixture f;
        CHECK(f.store->apply("system", std::nullopt, Json::parse(R"([{"op": "review_begin"}])"))[0]
                  .reason == "internal_error");
        CHECK(f.store->snapshot()["insight_review"]["status"] == "pending");
        REQUIRE(f.store
                    ->apply("system", std::nullopt,
                            Json::parse(R"([{"op": "review_begin", "run_id": "r"}])"))[0]
                    .ok);
        CHECK(f.store->snapshot()["insight_review"]["message"] ==
              "Checking the final insights\xE2\x80\xA6");
        CHECK(f.store
                  ->apply("system", std::nullopt,
                          Json::parse(R"([{"op": "review_begin", "run_id": "r2"}])"))[0]
                  .reason == "review_running");
        CHECK(f.store
                  ->apply("system", std::nullopt,
                          Json::parse(
                              R"([{"op": "review_finish", "run_id": "r2", "status": "ready"}])"))[0]
                  .reason == "stale_review");
    }
    SECTION("AI insights turned off during the run stop the review") {
        Fixture f;
        REQUIRE(f.store
                    ->apply("system", std::nullopt,
                            Json::parse(R"([{"op": "review_begin", "run_id": "r"}])"))[0]
                    .ok);
        REQUIRE(f.store
                    ->apply("host", std::nullopt,
                            Json::parse(R"([{"op": "set_cloud_enabled", "enabled": false}])"))[0]
                    .ok);
        const auto finished = f.store->apply(
            "system", std::nullopt,
            Json::parse(R"([{"op": "review_finish", "run_id": "r", "status": "ready"}])"))[0];
        CHECK(finished.ok);
        CHECK(f.store->snapshot()["insight_review"]["status"] == "unavailable");
        CHECK(f.store->snapshot()["insight_review"]["message"] ==
              "AI insights are off. Review was stopped.");
    }
    SECTION("a meeting still running is not ready") {
        Fixture f;
        REQUIRE(f.store->update_runtime_fields({{"status", "active"}}));
        CHECK(f.store
                  ->apply("system", std::nullopt,
                          Json::parse(R"([{"op": "review_begin", "run_id": "r"}])"))[0]
                  .reason == "meeting_not_ready");
    }
}
