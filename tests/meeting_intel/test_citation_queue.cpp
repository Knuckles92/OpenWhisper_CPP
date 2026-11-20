// The verifier tests of tests/test_meeting_fast_features.py (meeting/citation_verifier.py):
// advisory outcomes, the queue's order, stale results and quiet invalidation. OpenWhisper's
// ManualExecutor is the verifier's own worker, observed through busy(); its `invalidate()` is a
// host's user note, which makes the verifier invalidate every check; the judge is llm::FakeJudge
// marked device-local.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "llm/testing/fake_judge.h"
#include "meeting_intel/citation_verifier.h"
#include "meeting_state/fast_ops.h"
#include "rows_repo.h"

namespace {

using namespace std::chrono_literals;
using fv::Json;
using fv::llm::FakeJudge;
using fv::meeting_intel::CitationVerifier;
using fv::meeting_intel::test::eventually;
using fv::meeting_intel::test::RowsRepo;

Json fast_row() {
    return Json{{"id", "sg_one"},
                {"text", "We agree to ship the beta on Friday."},
                {"start_s", 10},
                {"end_s", 15}};
}

struct Verifying {
    explicit Verifying(bool allow = true) : allowed(allow) {
        judge->set_local(true);
        judge->set_model("jev-1.13.0");
        fv::meeting_state::MeetingState document;
        document.meeting_id = "m_test";
        document.cloud_enabled = true;
        state = std::make_shared<fv::meeting_state::MeetingStateStore>(std::move(document));
        verifier = std::make_unique<CitationVerifier>(
            state, repo,
            CitationVerifier::Deps{.judge = [judge = judge] { return judge; },
                                   .allowed =
                                       [this] {
                                           return allowed.load();
                                       }});
    }
    ~Verifying() { verifier->shutdown(); }

    Json add_claim(const std::string& text = "The beta ships on Friday.") {
        const auto results =
            state->apply("agent", std::string("notes"),
                         Json::array({Json{{"op", "add_item"},
                                           {"card", "decisions"},
                                           {"text", text},
                                           {"evidence", Json::array({"sg_one"})}}}));
        REQUIRE(results.size() == 1);
        REQUIRE(results[0].ok);
        return results[0].effect.at("item");
    }
    // OpenWhisper's verifier.invalidate(): a human term note invalidates every check.
    void invalidate_all() {
        REQUIRE(state
                    ->apply("host", std::string("me"),
                            Json::array({Json{{"op", "add_item"},
                                              {"card", "user_notes"},
                                              {"text", "Keep the launch notes short."}}}))[0]
                    .ok);
    }
    void idle() const {
        REQUIRE(eventually([this] { return !verifier->busy(); }));
    }
    Json decision(std::size_t index = 0) const {
        return state->snapshot().at("cards").at("decisions").at(index);
    }

    std::shared_ptr<RowsRepo> repo = std::make_shared<RowsRepo>(std::vector<Json>{fast_row()});
    std::shared_ptr<FakeJudge> judge = std::make_shared<FakeJudge>();
    std::atomic<bool> allowed;
    std::shared_ptr<fv::meeting_state::MeetingStateStore> state;
    std::unique_ptr<CitationVerifier> verifier;
};

TEST_CASE("verifier failure and missing evidence are advisory", "[citation_verifier]") {
    // from
    // tests/test_meeting_fast_features.py::test_verifier_failure_and_missing_evidence_are_advisory
    Verifying v; // the judge answers nothing
    v.add_claim();
    v.idle();
    CHECK(v.decision().at("citation_check").at("status") == "unavailable");
    CHECK(v.decision().at("status") == "proposed");
    v.repo->clear();
    v.invalidate_all();
    v.idle();
    CHECK(v.decision().at("citation_check").at("status") == "missing");
    CHECK(v.judge->calls().size() == 1);
}

TEST_CASE("verifier queue checks oldest first and refreshes queued items", "[citation_verifier]") {
    // from
    // tests/test_meeting_fast_features.py::test_verifier_queue_checks_oldest_first_and_refreshes_queued_items
    Verifying v;
    std::promise<void> entered;
    std::promise<void> release;
    auto holding = entered.get_future();
    auto go = release.get_future().share();
    std::atomic<int> asks{0};
    v.judge->set_handler([&, go](const Json&, const Json&) {
        if (++asks == 1) {
            entered.set_value();
            go.wait_for(3s);
        }
        return std::optional(FakeJudge::choice_answer("supported", 0.95));
    });
    v.add_claim("Claim zero.");
    REQUIRE(holding.wait_for(3s) == std::future_status::ready);
    const auto first = v.add_claim("Claim one, old.");
    v.add_claim("Claim two.");
    const auto updated =
        v.state->apply("agent", std::string("notes"),
                       Json::array({Json{{"op", "update_item"},
                                         {"id", first.at("id")},
                                         {"base_revision", first.at("revision")},
                                         {"evidence", Json::array({"sg_one"})},
                                         {"set", {{"text", "Claim one, new."}}}}}))[0];
    INFO(updated.reason.value_or(""));
    REQUIRE(updated.ok);
    release.set_value();
    v.idle();
    std::vector<std::string> claims;
    for (const auto& call : v.judge->calls()) {
        claims.push_back(call.state.at("claim").at("text").get<std::string>());
    }
    CHECK((claims == std::vector<std::string>{"Claim zero.", "Claim one, new.", "Claim two."}));
}

TEST_CASE("verifier rejects stale revision and changed source", "[citation_verifier]") {
    // from
    // tests/test_meeting_fast_features.py::test_verifier_rejects_stale_revision_and_changed_source
    Verifying v;
    v.judge->set_handler([&](const Json&, const Json&) {
        v.repo->set_text(0, "We did not agree.");
        return std::optional(FakeJudge::choice_answer("supported", 0.99));
    });
    const auto item = v.add_claim();
    v.idle();
    CHECK(v.judge->calls().size() == 1);
    CHECK(v.decision().at("citation_check").empty());
    REQUIRE(v.state
                ->apply("host", std::string("me"),
                        Json::array({Json{{"op", "update_item"},
                                          {"id", item.at("id")},
                                          {"set", {{"text", "A corrected claim"}}}}}))[0]
                .ok);
    const auto result =
        v.state->apply("system", std::string(fv::meeting_state::CITATION_VERIFIER_ACTOR),
                       Json::array({Json{{"op", "citation_check"},
                                         {"id", item.at("id")},
                                         {"revision", 1},
                                         {"check", {{"status", "supported"}}}}}));
    CHECK_FALSE(result[0].ok);
}

TEST_CASE("no advisory writes when no checks exist", "[citation_verifier]") {
    // from tests/test_meeting_fast_features.py::test_no_advisory_writes_when_no_checks_exist
    Verifying v(false);
    v.add_claim();
    v.idle();
    v.invalidate_all();
    const auto seq = v.state->seq();
    v.idle();
    CHECK(v.state->seq() == seq);
    CHECK(v.judge->calls().empty());
}

} // namespace
