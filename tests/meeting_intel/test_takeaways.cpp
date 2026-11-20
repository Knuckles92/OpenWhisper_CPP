// tests/test_meeting_takeaways.py: takeaways share the pulse consent, evidence and publication
// flow. Python's TypeSafeJudge over a scripted transport is a device-local FakeJudge here
// (a29-local-meeting-judgments); LiveSignals is LiveHighlights.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "llm/testing/fake_judge.h"
#include "meeting_intel/live_highlights.h"
#include "meeting_state/ids.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"
#include "rows_repo.h"

using fv::Json;
using fv::llm::FakeJudge;
using fv::meeting_intel::LiveHighlights;
using fv::meeting_intel::test::eventually;
using fv::meeting_intel::test::RowsRepo;

namespace {

const std::vector<std::pair<std::string, std::string>> kPassages{
    {"decision", "We agree to ship the beta."},
    {"disagreement", "Sales and engineering disagree about the scope."},
    {"commitment", "I will send the release notes by Friday."},
    {"number", "The budget is five thousand dollars."},
    {"takeaway", "We learned that early customer feedback prevents rework."},
};

std::shared_ptr<fv::meeting_state::MeetingStateStore>
store(std::function<bool(std::string_view)> exists = {}) {
    fv::meeting_state::MeetingState state;
    state.meeting_id = "m_test";
    state.cloud_enabled = true;
    fv::meeting_state::SegmentPredicates predicates;
    predicates.segment_exists = std::move(exists);
    return std::make_shared<fv::meeting_state::MeetingStateStore>(
        std::move(state), nullptr, fv::meeting_state::SegmentHandler{}, fv::system_clock(),
        fv::meeting_state::random_ids(), std::move(predicates));
}

fv::meeting_state::OpResult publish(fv::meeting_state::MeetingStateStore& target, int minute,
                                    const Json& pulses, std::string actor = "system",
                                    std::string actor_id = "live-signals") {
    return target.apply(
        actor, actor_id,
        Json::array(
            {Json{{"op", "publish_highlights"}, {"minute", minute}, {"pulses", pulses}}}))[0];
}

} // namespace

// from tests/test_meeting_takeaways.py::test_all_five_pulse_types_publish_in_one_validated_request
TEST_CASE("test_all_five_pulse_types_publish_in_one_validated_request",
          "[test_meeting_takeaways]") {
    std::vector<Json> rows;
    for (std::size_t index = 0; index < kPassages.size(); ++index) {
        rows.push_back(Json{{"id", "sg_" + kPassages[index].first},
                            {"text", kPassages[index].second},
                            {"start_s", 5.0 + index * 10},
                            {"end_s", 8.0 + index * 10}});
    }
    auto repo = std::make_shared<RowsRepo>(rows);
    auto state =
        store([repo](std::string_view id) { return repo->get_segment("m_test", id).has_value(); });
    auto judge = std::make_shared<FakeJudge>();
    judge->set_local(true);
    judge->set_handler([](const Json& request, const Json& questions) {
        std::set<std::string> expected;
        for (const auto& [kind, text] : kPassages) {
            expected.insert(kind);
            expected.insert(kind + "_anchor");
        }
        std::set<std::string> asked;
        for (const auto& [key, value] : questions.items()) {
            asked.insert(key);
        }
        CHECK(asked == expected);
        CHECK(questions.at("takeaway").at("type") == "noul");
        CHECK(questions.at("takeaway_anchor").at("criteria").contains("none"));
        Json answers = Json::object();
        for (const auto& [kind, text] : kPassages) {
            answers[kind] = Json{{"type", "noul"}, {"noul", 0.93}};
            Json probabilities = Json::object();
            for (const auto& [sid, passage] : request.at("passages").items()) {
                probabilities[sid] = sid == "sg_" + kind ? 1.0 : 0.0;
            }
            probabilities["none"] = 0.0;
            answers[kind + "_anchor"] = Json{{"type", "choice"},
                                             {"choice", "sg_" + kind},
                                             {"confidence", 0.9},
                                             {"probabilities", probabilities}};
        }
        return std::optional(answers);
    });
    LiveHighlights worker(state, repo,
                          {.judge = [judge] { return judge; },
                           .enabled = [] { return true; },
                           .radar_enabled =
                               [] {
                                   return false;
                               }});
    worker.observe({}, 60);
    REQUIRE(eventually([&] { return !worker.busy(); }));
    const Json pulses = state->snapshot().at("live_highlights");
    CHECK(judge->calls().size() == 1);
    REQUIRE(pulses.size() == kPassages.size());
    std::set<std::string> kinds;
    for (std::size_t index = 0; index < pulses.size(); ++index) {
        const Json& pulse = pulses[index];
        kinds.insert(pulse.at("kind").get<std::string>());
        CHECK(pulse.at("segment_id") == rows[index].at("id"));
        CHECK(pulse.at("text") == rows[index].at("text"));
        CHECK(pulse.at("start_s") == rows[index].at("start_s"));
        CHECK(pulse.at("id") == std::format("pulse_0_{}", pulse.at("kind").get<std::string>()));
        // dict.fromkeys(passages, .93): compared as a mapping, in any order.
        std::map<std::string, double> scores;
        for (const auto& [kind, text] : kPassages) {
            scores[kind] = 0.93;
        }
        CHECK(pulse.at("assessment").at("scores").get<std::map<std::string, double>>() == scores);
        CHECK(pulse.at("assessment").at("source_probability") == 1);
    }
    CHECK(kinds.size() == kPassages.size());
    worker.observe(rows, 0);
    REQUIRE(eventually([&] { return !worker.busy(); }));
    worker.shutdown();
    CHECK(judge->calls().size() == 1);
    CHECK(state->snapshot().at("live_highlights") == pulses);
}

// from tests/test_meeting_takeaways.py::test_takeaway_is_not_requested_when_highlights_are_disabled
TEST_CASE("test_takeaway_is_not_requested_when_highlights_are_disabled",
          "[test_meeting_takeaways]") {
    auto repo = std::make_shared<RowsRepo>(
        std::vector<Json>{Json{{"id", "sg_one"},
                               {"text", "What did we learn from the launch?"},
                               {"start_s", 10},
                               {"end_s", 15}}});
    auto state = store();
    auto judge = std::make_shared<FakeJudge>();
    judge->set_local(true);
    LiveHighlights worker(state, repo,
                          {.judge = [judge] { return judge; },
                           .enabled = [] { return false; },
                           .radar_enabled =
                               [] {
                                   return true;
                               }});
    worker.observe({}, 60);
    REQUIRE(eventually([&] { return !worker.busy(); }));
    worker.shutdown();
    const auto calls = judge->calls();
    REQUIRE(calls.size() == 1);
    std::set<std::string> asked;
    for (const auto& [key, value] : calls[0].questions.items()) {
        asked.insert(key);
    }
    CHECK(asked == std::set<std::string>{"candidate_0"});
}

// from tests/test_meeting_takeaways.py::test_takeaway_publication_remains_system_owned
TEST_CASE("test_takeaway_publication_remains_system_owned", "[test_meeting_takeaways]") {
    const auto [actor, actor_id] = GENERATE(std::pair<std::string, std::string>{"host", "me"},
                                            std::pair<std::string, std::string>{"agent", "notes"},
                                            std::pair<std::string, std::string>{"system", "other"});
    auto state = store();
    const auto result = state->apply(
        actor, actor_id,
        Json::array({Json{
            {"op", "publish_highlights"},
            {"pulses", Json::array({Json{{"id", "pulse_0_takeaway"},
                                         {"kind", "takeaway"},
                                         {"start_s", 10},
                                         {"segment_id", "sg_one"},
                                         {"probability", 0.93},
                                         {"text", "Early feedback prevents rework."}}})}}}))[0];
    CHECK_FALSE(result.ok);
    CHECK(state->snapshot().at("live_highlights").empty());
}

// from
// tests/test_meeting_takeaways.py::test_takeaway_replacement_does_not_leave_old_sources_or_clear_other_minutes
TEST_CASE("test_takeaway_replacement_does_not_leave_old_sources_or_clear_other_minutes",
          "[test_meeting_takeaways]") {
    auto state = store([](std::string_view id) { return id == "old" || id == "new"; });
    const Json pulse{{"id", "pulse_0_takeaway"},
                     {"kind", "takeaway"},
                     {"start_s", 10},
                     {"segment_id", "old"},
                     {"probability", 0.93},
                     {"text", "Early feedback prevents rework."}};
    CHECK(publish(*state, 0, Json::array({pulse})).ok);
    Json later = pulse;
    later["id"] = "pulse_1_takeaway";
    later["start_s"] = 70;
    CHECK(publish(*state, 1, Json::array({later})).ok);
    Json replacement = pulse;
    replacement["segment_id"] = "new";
    replacement["start_s"] = 20;
    replacement["text"] = "Smaller launches reduce risk.";
    CHECK(publish(*state, 0, Json::array({replacement})).ok);
    CHECK(state->snapshot().at("live_highlights") == Json::array({replacement, later}));
    Json missing = replacement;
    missing["segment_id"] = "missing";
    CHECK_FALSE(publish(*state, 0, Json::array({missing})).ok);
    Json unknown = replacement;
    unknown["kind"] = "unknown";
    CHECK_FALSE(publish(*state, 0, Json::array({unknown})).ok);
    Json six = Json::array();
    for (int i = 0; i < 6; ++i) {
        six.push_back(replacement);
    }
    CHECK_FALSE(publish(*state, 0, six).ok);
    CHECK(state->snapshot().at("live_highlights") == Json::array({replacement, later}));
    CHECK(publish(*state, 0, Json::array()).ok);
    CHECK(state->snapshot().at("live_highlights") == Json::array({later}));
}
