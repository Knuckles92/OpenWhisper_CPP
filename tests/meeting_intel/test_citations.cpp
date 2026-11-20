#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>

#include "llm/testing/fake_judge.h"
#include "meeting_intel/citation_verifier.h"
#include "meeting_state/testing/fake_meeting_repository.h"

namespace {

using fv::Json;
using namespace std::chrono_literals;

struct CitationFixture {
    std::shared_ptr<fv::meeting_state::testing::FakeMeetingRepository> repo =
        std::make_shared<fv::meeting_state::testing::FakeMeetingRepository>();
    std::shared_ptr<fv::meeting_state::MeetingStateStore> state;

    CitationFixture() {
        repo->create_meeting(Json{{"id", "m_citations"},
                                  {"status", "active"},
                                  {"started_at", "2026-11-17T09:00:00+00:00"},
                                  {"host_token", "h"},
                                  {"guest_token", "g"},
                                  {"spool_dir", "spool"}});
        fv::meeting_state::TranscriptSegment segment;
        segment.segment_id = "sg_beta";
        segment.meeting_id = "m_citations";
        segment.channel = "mic";
        segment.start_s = 1;
        segment.end_s = 3;
        segment.text = "We agreed to ship the beta on Friday.";
        repo->add_segments({segment});
        fv::meeting_state::MeetingState document;
        document.meeting_id = "m_citations";
        document.cloud_enabled = true;
        state = std::make_shared<fv::meeting_state::MeetingStateStore>(std::move(document));
    }

    Json add_claim() {
        const auto results =
            state->apply("agent", std::string("notes"),
                         Json::array({Json{{"op", "add_item"},
                                           {"card", "decisions"},
                                           {"text", "The beta ships on Friday."},
                                           {"evidence", Json::array({"sg_beta"})}}}));
        REQUIRE(results.size() == 1);
        REQUIRE(results.front().ok);
        return results.front().effect.at("item");
    }
};

TEST_CASE("local citation judgments annotate a proposed revision with cited speech",
          "[citation_verifier]") {
    CitationFixture fixture;
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    judge->set_local(true);
    judge->set_model("local-script");
    judge->push(fv::llm::FakeJudge::choice_answer("supported", 0.95));
    std::promise<Json> annotated;
    auto done = annotated.get_future();
    auto listener = fixture.state->subscribe([&](std::int64_t, const auto& ops) {
        for (const auto& op : ops) {
            if (op.op.value("op", "") == "citation_check") {
                annotated.set_value(op.effect.at("item"));
            }
        }
    });
    fv::meeting_intel::CitationVerifier verifier(fixture.state, fixture.repo,
                                                 {.judge = [judge] { return judge; },
                                                  .allowed =
                                                      [] {
                                                          return true;
                                                      }});
    const auto original = fixture.add_claim();
    REQUIRE(done.wait_for(2s) == std::future_status::ready);
    const auto verified = done.get();
    verifier.shutdown();
    CHECK(verified.at("revision") == original.at("revision"));
    CHECK(verified.at("status") == original.at("status"));
    CHECK(verified.at("citation_check").at("status") == "supported");
    CHECK(verified.at("citation_check").at("model") == "local-script");
    const auto calls = judge->calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls.front().state.at("citations").size() == 1);
    CHECK(calls.front().state.at("citations")[0].at("text") ==
          "We agreed to ship the beta on Friday.");
}

TEST_CASE("citation verification does not send evidence to a non-local judge",
          "[citation_verifier]") {
    CitationFixture fixture;
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    std::promise<void> lookedUp;
    auto lookup = lookedUp.get_future();
    fv::meeting_intel::CitationVerifier verifier(fixture.state, fixture.repo,
                                                 {.judge =
                                                      [&] {
                                                          lookedUp.set_value();
                                                          return judge;
                                                      },
                                                  .allowed =
                                                      [] {
                                                          return true;
                                                      }});
    fixture.add_claim();
    REQUIRE(lookup.wait_for(2s) == std::future_status::ready);
    verifier.shutdown();
    CHECK(judge->calls().empty());
    const auto item = fixture.state->snapshot().at("cards").at("decisions")[0];
    CHECK(item.at("citation_check").empty());
}

TEST_CASE("revoking a local citation judgment discards the in-flight result",
          "[citation_verifier]") {
    CitationFixture fixture;
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    judge->set_local(true);
    std::atomic<bool> allowed{true};
    std::promise<void> entered;
    std::promise<void> finish;
    auto judging = entered.get_future();
    auto release = finish.get_future();
    judge->set_handler([&](const Json&, const Json&) -> std::optional<Json> {
        entered.set_value();
        if (release.wait_for(2s) != std::future_status::ready) {
            return std::nullopt;
        }
        return fv::llm::FakeJudge::choice_answer("supported", 0.95);
    });
    fv::meeting_intel::CitationVerifier verifier(fixture.state, fixture.repo,
                                                 {.judge = [judge] { return judge; },
                                                  .allowed =
                                                      [&] {
                                                          return allowed.load();
                                                      }});
    fixture.add_claim();
    REQUIRE(judging.wait_for(2s) == std::future_status::ready);
    allowed.store(false);
    finish.set_value();
    verifier.shutdown();
    CHECK(judge->calls().size() == 1);
    CHECK(fixture.state->snapshot().at("cards").at("decisions")[0].at("citation_check").empty());
}

} // namespace
