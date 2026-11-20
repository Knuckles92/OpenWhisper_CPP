#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <memory>

#include "llm/testing/fake_judge.h"
#include "meeting_intel/live_highlights.h"
#include "meeting_state/testing/fake_meeting_repository.h"

namespace {

using fv::Json;
using namespace std::chrono_literals;

struct HighlightFixture {
    std::shared_ptr<fv::meeting_state::testing::FakeMeetingRepository> repo =
        std::make_shared<fv::meeting_state::testing::FakeMeetingRepository>();
    std::shared_ptr<fv::meeting_state::MeetingStateStore> state;

    HighlightFixture() {
        repo->create_meeting(Json{{"id", "m_highlights"},
                                  {"status", "active"},
                                  {"started_at", "2026-11-17T09:00:00+00:00"},
                                  {"host_token", "h"},
                                  {"guest_token", "g"},
                                  {"spool_dir", "spool"}});
        fv::meeting_state::MeetingState document;
        document.meeting_id = "m_highlights";
        state = std::make_shared<fv::meeting_state::MeetingStateStore>(std::move(document));
    }

    void segment(std::string id, double start, std::string text) {
        fv::meeting_state::TranscriptSegment row;
        row.segment_id = std::move(id);
        row.meeting_id = "m_highlights";
        row.channel = "mic";
        row.start_s = start;
        row.end_s = start + 2;
        row.text = std::move(text);
        repo->add_segments({row});
    }
};

Json decision(std::string id, Json probabilities = Json::object()) {
    return {{"decision", {{"noul", 0.94}}},
            {"decision_anchor",
             {{"choice", std::move(id)},
              {"confidence", 0.88},
              {"probabilities", std::move(probabilities)},
              {"text", "Invented content"}}}};
}

TEST_CASE("live highlights copy source text and rank the complete source distribution",
          "[live_highlights]") {
    HighlightFixture fixture;
    fixture.segment("sg_topic", 2, "The meeting opens.");
    fixture.segment("sg_decision", 12, "We agreed to launch next Friday.");
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    judge->set_local(true);
    judge->push(
        decision("sg_decision", Json{{"sg_decision", 0.9}, {"sg_topic", 0.08}, {"none", 0.02}}));
    std::promise<Json> published;
    auto result = published.get_future();
    auto sub = fixture.state->subscribe([&](std::int64_t, const auto& ops) {
        for (const auto& op : ops) {
            if (op.op.value("op", "") == "publish_highlights") {
                published.set_value(op.effect.at("pulses"));
            }
        }
    });
    fv::meeting_intel::LiveHighlights highlights(fixture.state, fixture.repo,
                                                 {.judge = [judge] { return judge; },
                                                  .enabled =
                                                      [] {
                                                          return true;
                                                      }});
    highlights.observe({}, 60);
    REQUIRE(result.wait_for(2s) == std::future_status::ready);
    const auto pulses = result.get();
    highlights.shutdown();
    REQUIRE(pulses.size() == 1);
    CHECK(pulses[0].at("segment_id") == "sg_decision");
    CHECK(pulses[0].at("start_s") == 12.0);
    CHECK(pulses[0].at("text") == "We agreed to launch next Friday.");
    const auto& assessment = pulses[0].at("assessment");
    CHECK(assessment.at("scores").at("decision") == 0.94);
    CHECK(assessment.at("source_rank") == 1);
    CHECK(assessment.at("source_option_count") == 3);
    CHECK(assessment.at("source_options")[0].at("text") == "We agreed to launch next Friday.");
}

TEST_CASE("final local highlights catch up the partial last minute", "[live_highlights]") {
    HighlightFixture fixture;
    fixture.segment("sg_earlier", 5, "Agenda discussion.");
    fixture.segment("sg_last", 65, "Everyone accepted the revised release schedule.");
    REQUIRE(fixture.state->update_runtime_fields(Json{{"status", "ended"}}));
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    judge->set_local(true);
    judge->push(decision("sg_last"));
    judge->push(decision("sg_unknown"));
    fv::meeting_intel::LiveHighlights highlights(fixture.state, fixture.repo,
                                                 {.judge = [judge] { return judge; },
                                                  .enabled =
                                                      [] {
                                                          return true;
                                                      }});
    highlights.finalize();
    const auto calls = judge->calls();
    REQUIRE(calls.size() == 2);
    CHECK(calls.front().state.at("passages").contains("sg_last"));
    const auto pulses = fixture.state->snapshot().at("live_highlights");
    REQUIRE(pulses.size() == 1);
    CHECK(pulses[0].at("segment_id") == "sg_last");
}

TEST_CASE("local highlights never ask a non-local judge", "[live_highlights]") {
    HighlightFixture fixture;
    fixture.segment("sg_last", 65, "Everyone accepted the revised release schedule.");
    REQUIRE(fixture.state->update_runtime_fields(Json{{"status", "ended"}}));
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    fv::meeting_intel::LiveHighlights highlights(fixture.state, fixture.repo,
                                                 {.judge = [judge] { return judge; },
                                                  .enabled =
                                                      [] {
                                                          return true;
                                                      }});
    highlights.finalize();
    CHECK(judge->calls().empty());
    CHECK(fixture.state->snapshot().at("live_highlights").empty());
}

TEST_CASE("local question radar cites the exact question and suggests a quoted answer",
          "[live_highlights]") {
    HighlightFixture fixture;
    fixture.segment("sg_duplicate", 11, "Who will publish the roadmap?");
    fixture.segment("sg_question", 12, "When will we publish the roadmap?");
    fixture.segment("sg_answer", 15, "We will release the hiring plan on Thursday.");
    const auto seeded = fixture.state->apply(
        "host", std::nullopt,
        Json::array({Json{{"op", "ask_question"}, {"text", "When is the hiring plan due?"}}}));
    REQUIRE(seeded.size() == 1);
    REQUIRE(seeded.front().ok);
    const auto id = fixture.state->snapshot().at("questions")[0].at("id").get<std::string>();
    const auto duplicate = fixture.state->apply(
        "host", std::nullopt,
        Json::array({Json{{"op", "ask_question"}, {"text", "Who will publish the roadmap?"}}}));
    REQUIRE(duplicate.front().ok);
    const auto dismissed_id = fixture.state->snapshot().at("questions")[1].at("id");
    REQUIRE(
        fixture.state
            ->apply("host", std::nullopt,
                    Json::array({Json{{"op", "dismiss_question"}, {"question_id", dismissed_id}}}))
            .front()
            .ok);
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    judge->set_local(true);
    judge->push(
        Json{{"candidate_0", {{"noul", 0.99}}},
             {"candidate_1", {{"noul", 0.98}, {"text", "Invented question"}}},
             {"answer_" + id,
              {{"choice", "sg_answer"}, {"confidence", 0.96}, {"text", "Invented answer"}}}});
    std::promise<void> published;
    auto result = published.get_future();
    auto sub = fixture.state->subscribe([&](std::int64_t, const auto& ops) {
        for (const auto& op : ops) {
            if (op.op.value("op", "") == "resolve_question") {
                published.set_value();
            }
        }
    });
    fv::meeting_intel::LiveHighlights radar(fixture.state, fixture.repo,
                                            {.judge = [judge] { return judge; },
                                             .radar_enabled =
                                                 [] {
                                                     return true;
                                                 }});
    radar.observe({}, 60);
    REQUIRE(result.wait_for(2s) == std::future_status::ready);
    radar.shutdown();
    const auto snapshot = fixture.state->snapshot();
    const auto& questions = snapshot.at("questions");
    REQUIRE(questions.size() == 3);
    CHECK(questions[0].at("status") == "open");
    CHECK(questions[0].at("suggested_answer") == "We will release the hiring plan on Thursday.");
    CHECK(questions[1].at("status") == "dismissed");
    CHECK(questions[2].at("text") == "When will we publish the roadmap?");
    CHECK(questions[2].at("evidence") == Json::array({"sg_question"}));
    CHECK(snapshot.at("live_highlights").empty());
    REQUIRE(judge->calls().size() == 1);
    CHECK_FALSE(judge->calls()[0].questions.contains("decision"));
}

} // namespace
