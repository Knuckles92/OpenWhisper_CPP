// tests/test_meeting_repair.py tests 1-22: deterministic meeting-state repairs. The evidence-id
// repair of the agent's tool policy (the other 9) is M8's.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "meeting_state/content.h"
#include "meeting_state/repair.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"
#include "meeting_state/testing/fixed_ids.h"

using fv::Json;
using namespace fv::meeting_state;

namespace {

std::vector<Json> rows(const char* json) {
    std::vector<Json> out;
    for (const auto& row : Json::parse(json)) {
        out.push_back(row);
    }
    return out;
}

bool contains(const Json& text, std::string_view needle) {
    return text.get<std::string>().find(needle) != std::string::npos;
}

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

struct Fixture {
    fv::FakeClock clock;
    testing::FixedIds ids;
    MeetingStateStore store;

    explicit Fixture(MeetingState state) : store(std::move(state), nullptr, {}, clock, ids) {}

    void apply(const std::string& actor_type, const std::string& actor_id, const char* ops) {
        for (const auto& result : store.apply(actor_type, actor_id, Json::parse(ops))) {
            REQUIRE(result.ok);
        }
    }
};

MeetingState state_for(const std::string& meeting_id, const std::string& status = "active") {
    MeetingState state;
    state.meeting_id = meeting_id;
    state.status = status;
    return state;
}

std::vector<Json> live(const Json& items) {
    std::vector<Json> out;
    for (const auto& item : items) {
        if (item.value("status", "") != "removed") {
            out.push_back(item);
        }
    }
    return out;
}

} // namespace

// from tests/test_meeting_repair.py::test_build_ops_noop_when_timeline_populated
TEST_CASE("test_build_ops_noop_when_timeline_populated", "[test_meeting_repair]") {
    const Json state = Json::parse(R"({"cards": {"key_points": [], "timeline": [
        {"id": "it_1", "text": "Already there", "status": "proposed",
         "data": {"start_s": 0.0}, "evidence": ["sg_1"]}]}})");
    CHECK(build_timeline_backfill_ops(state, {}) == Json::array());
}

// from tests/test_meeting_repair.py::test_build_ops_promotes_key_points_with_evidence
TEST_CASE("test_build_ops_promotes_key_points_with_evidence", "[test_meeting_repair]") {
    const Json state = Json::parse(R"({"cards": {"key_points": [
        {"id": "it_a", "text": "Apple example", "status": "proposed",
         "evidence": ["sg_late", "sg_early"]},
        {"id": "it_b", "text": "No anchors", "status": "proposed", "evidence": []}],
        "timeline": []}})");
    const Json ops = build_timeline_backfill_ops(state, rows(R"([
        {"id": "sg_early", "start_s": 5.0, "text": "early"},
        {"id": "sg_late", "start_s": 40.0, "text": "late"}])"));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0]["card"] == "timeline");
    CHECK(ops[0]["text"] == "Apple example");
    CHECK(ops[0]["data"]["start_s"] == 5.0);
}

// from tests/test_meeting_repair.py::test_build_ops_does_not_invent_opening_beat_from_raw_speech
TEST_CASE("test_build_ops_does_not_invent_opening_beat_from_raw_speech", "[test_meeting_repair]") {
    const Json state = Json::parse(R"({"cards": {"key_points": [
        {"id": "it_a", "text": "Apple example", "status": "proposed", "evidence": ["sg_apple"]}],
        "timeline": []}})");
    const Json ops = build_timeline_backfill_ops(state, rows(R"([
        {"id": "sg_open", "start_s": 0.0, "text": "How do you explain assumptions?"},
        {"id": "sg_apple", "start_s": 27.7, "text": "Why is Apple innovative?"}])"));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0]["data"]["start_s"] == 27.7);
    CHECK(ops[0]["evidence"] == Json::array({"sg_apple"}));
}

// from tests/test_meeting_repair.py::test_build_ops_does_not_sample_transcript_windows
TEST_CASE("test_build_ops_does_not_sample_transcript_windows", "[test_meeting_repair]") {
    const Json state = Json::parse(R"({"cards": {"key_points": [], "timeline": []}})");
    CHECK(build_timeline_backfill_ops(state, rows(R"([
        {"id": "sg_0", "start_s": 0.0, "text": "Opening question"},
        {"id": "sg_1", "start_s": 8.0, "text": "Still opening"},
        {"id": "sg_2", "start_s": 25.0, "text": "Middle example"},
        {"id": "sg_3", "start_s": 50.0, "text": "Closing discovery"}])")) == Json::array());
}

// from tests/test_meeting_repair.py::test_build_summary_ops_from_key_points_when_empty
TEST_CASE("test_build_summary_ops_from_key_points_when_empty", "[test_meeting_repair]") {
    const Json state = Json::parse(R"({"rolling_summary": "", "cards": {"key_points": [
        {"text": "Apple innovates", "status": "proposed", "evidence": ["sg_1"]},
        {"text": "Wright brothers flew", "status": "proposed", "evidence": ["sg_2"]}]}})");
    const Json ops = build_summary_backfill_ops(state, {});
    REQUIRE(ops.size() == 1);
    CHECK(ops[0]["op"] == "set_rolling_summary");
    CHECK(contains(ops[0]["text"], "Apple innovates"));
    CHECK(contains(ops[0]["text"], "Wright brothers flew"));
}

// from tests/test_meeting_repair.py::test_build_summary_ops_noop_when_present
TEST_CASE("test_build_summary_ops_noop_when_present", "[test_meeting_repair]") {
    const Json state =
        Json::parse(R"({"rolling_summary": "Already written.", "cards": {"key_points": []}})");
    CHECK(build_summary_backfill_ops(state, {}) == Json::array());
}

// from tests/test_meeting_repair.py::test_build_topic_ops_from_first_key_point
TEST_CASE("test_build_topic_ops_from_first_key_point", "[test_meeting_repair]") {
    const Json state = Json::parse(R"({"topic": {"current": ""}, "cards": {"key_points": [
        {"text": "Why do some defy assumptions?", "status": "proposed", "evidence": ["sg_1"]}]}})");
    const Json ops = build_topic_backfill_ops(state, {});
    REQUIRE(ops.size() == 1);
    CHECK(ops[0]["op"] == "set_topic");
    CHECK(lowered(ops[0]["text"].get<std::string>()).find("assumptions") != std::string::npos);
}

// from tests/test_meeting_repair.py::test_repair_meeting_state_applies_through_store
TEST_CASE("test_repair_meeting_state_applies_through_store", "[test_meeting_repair]") {
    Fixture f(state_for("m_repair"));
    f.apply("agent", "agent", R"([{"op": "add_item", "card": "key_points",
        "text": "Ship Friday", "evidence": ["sg_1"]}])");
    // Timeline beat + summary + topic composed from the key point.
    const int applied = repair_meeting_state(
        f.store, rows(R"([{"id": "sg_1", "start_s": 3.0, "text": "Ship Friday please"}])"));
    CHECK(applied == 3);
    const Json snap = f.store.snapshot();
    const auto timeline = live(snap["cards"]["timeline"]);
    REQUIRE(timeline.size() == 1);
    CHECK(timeline[0]["data"]["start_s"] == 3.0);
    CHECK(timeline[0]["author_type"] == "system");
    CHECK(contains(snap["rolling_summary"], "Ship Friday"));
    CHECK(snap["title"] == "");
}

// from tests/test_meeting_repair.py::test_build_untitled_title_ops_copies_topic_when_blank
TEST_CASE("test_build_untitled_title_ops_copies_topic_when_blank", "[test_meeting_repair]") {
    CHECK(build_untitled_title_ops(
              Json::parse(R"({"title": "", "topic": {"current": "Q3 roadmap and hiring"}})")) ==
          Json::parse(R"([{"op": "set_title", "text": "Q3 roadmap and hiring"}])"));
    CHECK(build_untitled_title_ops(
              Json::parse(R"({"title": "Planning sync", "topic": {"current": "Q3 roadmap"}})")) ==
          Json::array());
}

// from tests/test_meeting_repair.py::test_repair_copies_topic_into_blank_title_when_ended
TEST_CASE("test_repair_copies_topic_into_blank_title_when_ended", "[test_meeting_repair]") {
    MeetingState state = state_for("m_named", "ended");
    state.topic.current = "Q3 roadmap";
    Fixture f(state);
    CHECK(repair_meeting_state(f.store, {}) == 1);
    CHECK(f.store.snapshot()["title"] == "Q3 roadmap");
}

// from tests/test_meeting_repair.py::test_repair_does_not_overwrite_human_title
TEST_CASE("test_repair_does_not_overwrite_human_title", "[test_meeting_repair]") {
    MeetingState state = state_for("m_named", "ended");
    state.title = "Planning sync";
    state.topic.current = "Q3 roadmap";
    Fixture f(state);
    CHECK(repair_meeting_state(f.store, {}) == 0);
    CHECK(f.store.snapshot()["title"] == "Planning sync");
}

// from tests/test_meeting_repair.py::test_repair_does_not_promote_timeline_labels_to_key_points
TEST_CASE("test_repair_does_not_promote_timeline_labels_to_key_points", "[test_meeting_repair]") {
    Fixture f(state_for("m_labels"));
    f.apply("system", "state_repair", R"([
        {"op": "add_item", "card": "timeline", "text": "Seasoning.",
         "data": {"start_s": 71.0}, "evidence": ["sg_1"]},
        {"op": "add_item", "card": "timeline", "text": "Let's take a look at this.",
         "data": {"start_s": 45.0}, "evidence": ["sg_2"]}])");
    CHECK(repair_meeting_state(f.store, rows(R"([
        {"id": "sg_1", "start_s": 71.0, "text": "Seasoning."},
        {"id": "sg_2", "start_s": 45.0, "text": "Let's take a look at this."}])")) == 0);
    CHECK(f.store.snapshot()["cards"]["key_points"] == Json::array());
}

// from tests/test_meeting_repair.py::test_timeline_coverage_adds_missing_key_point_beats
TEST_CASE("test_timeline_coverage_adds_missing_key_point_beats", "[test_meeting_repair]") {
    const Json state = Json::parse(R"({"cards": {"key_points": [
        {"text": "Opening puzzle about assumptions", "status": "proposed", "evidence": ["sg_0"]},
        {"text": "For example, why is Apple so innovative?", "status": "proposed",
         "evidence": ["sg_a"]}],
        "timeline": [{"text": "Opening puzzle about assumptions", "status": "proposed",
                      "evidence": ["sg_0"], "data": {"start_s": 0.0}}]}})");
    const Json ops = build_timeline_coverage_from_segments(state, rows(R"([
        {"id": "sg_0", "start_s": 0.0, "text": "Opening"},
        {"id": "sg_a", "start_s": 27.0, "text": "Apple"}])"));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0]["data"]["start_s"] == 27.0);
    CHECK(lowered(ops[0]["text"].get<std::string>()).find("apple") != std::string::npos);
}

// from tests/test_meeting_repair.py::test_repair_leaves_raw_transcript_for_agent_synthesis
TEST_CASE("test_repair_leaves_raw_transcript_for_agent_synthesis", "[test_meeting_repair]") {
    Fixture f(state_for("m_raw", "ended"));
    CHECK(repair_meeting_state(f.store, rows(R"([
        {"id": "sg_0", "start_s": 0.0, "text": "Let's take a look at this."},
        {"id": "sg_1", "start_s": 25.0, "text": "Seasoning."},
        {"id": "sg_2", "start_s": 50.0, "text": "For example, why is Apple so innovative?"},
        {"id": "sg_3", "start_s": 75.0, "text": "Oh I love it."}])")) == 0);
    const Json snapshot = f.store.snapshot();
    for (const auto& [card, items] : snapshot["cards"].items()) {
        CHECK(items.empty());
    }
    CHECK(snapshot["rolling_summary"] == "");
    CHECK(snapshot["topic"]["current"] == "");
    CHECK(snapshot["title"] == "");
}

// from tests/test_meeting_repair.py::test_repair_does_not_recycle_legacy_key_point_samples
TEST_CASE("test_repair_does_not_recycle_legacy_key_point_samples", "[test_meeting_repair]") {
    Fixture f(state_for("m_legacy"));
    f.apply("system", "state_repair", R"([{"op": "add_item", "card": "key_points",
        "text": "Let's take a look at this.", "evidence": ["sg_1"]}])");
    CHECK(repair_meeting_state(
              f.store,
              rows(R"([{"id": "sg_1", "start_s": 1.0, "text": "Let's take a look at this."}])")) ==
          0);
    CHECK(f.store.snapshot()["rolling_summary"] == "");
    CHECK(f.store.snapshot()["cards"]["timeline"] == Json::array());
}

// from tests/test_meeting_repair.py::test_repair_meeting_state_fills_summary_only
TEST_CASE("test_repair_meeting_state_fills_summary_only", "[test_meeting_repair]") {
    Fixture f(state_for("m_repair"));
    f.apply("system", "seed", R"([
        {"op": "add_item", "card": "timeline", "text": "Opening",
         "data": {"start_s": 0.0}, "evidence": ["sg_1"]},
        {"op": "add_item", "card": "key_points", "text": "A finding", "evidence": ["sg_1"]}])");
    // Summary + topic (timeline already populated).
    CHECK(repair_meeting_state(f.store,
                               rows(R"([{"id": "sg_1", "start_s": 0.0, "text": "hello"}])")) == 2);
    const Json snap = f.store.snapshot();
    CHECK(contains(snap["rolling_summary"], "A finding"));
    CHECK_FALSE(snap["topic"]["current"].get<std::string>().empty());
}

// from tests/test_meeting_repair.py::test_repair_skips_timeline_when_ribbon_disabled
TEST_CASE("test_repair_skips_timeline_when_ribbon_disabled", "[test_meeting_repair]") {
    MeetingState state = state_for("m_repair_noribbon");
    state.report_views = {"brief"};
    Fixture f(state);
    f.apply("agent", "agent", R"([{"op": "add_item", "card": "key_points",
        "text": "The benchmarks held under peak load.", "evidence": ["sg_2"]}])");
    const int applied = repair_meeting_state(f.store, rows(R"([
        {"id": "sg_1", "start_s": 0.0, "text": "Let's start with the migration."},
        {"id": "sg_2", "start_s": 25.0, "text": "The benchmarks held under peak load."}])"));
    const Json snap = f.store.snapshot();
    CHECK(live(snap["cards"]["timeline"]).empty());
    CHECK(applied >= 1);
    CHECK((!snap["rolling_summary"].get<std::string>().empty() ||
           !snap["topic"]["current"].get<std::string>().empty()));
}

// from tests/test_meeting_repair.py::test_build_summary_ops_from_live_notes_when_key_points_empty
TEST_CASE("test_build_summary_ops_from_live_notes_when_key_points_empty", "[test_meeting_repair]") {
    const Json state = Json::parse(R"({"rolling_summary": "", "cards": {"key_points": [],
        "live_notes": [
          {"text": "Team reviewed Q3 metrics and conversion rates.", "status": "proposed",
           "data": {"heading": "Funnel metrics", "start_s": 12.0}, "evidence": ["sg_1"]},
          {"text": "Agreed to migrate OAuth provider by next sprint.", "status": "proposed",
           "data": {"heading": "OAuth Migration", "start_s": 45.0}, "evidence": ["sg_2"]}]}})");
    const Json ops = build_summary_backfill_ops(state, {});
    REQUIRE(ops.size() == 1);
    CHECK(ops[0]["op"] == "set_rolling_summary");
    CHECK(contains(ops[0]["text"], "Q3 metrics"));
    CHECK(contains(ops[0]["text"], "OAuth provider"));
}

// from tests/test_meeting_repair.py::test_build_topic_ops_from_live_notes_when_key_points_empty
TEST_CASE("test_build_topic_ops_from_live_notes_when_key_points_empty", "[test_meeting_repair]") {
    const Json state = Json::parse(R"({"topic": {"current": ""}, "cards": {"key_points": [],
        "live_notes": [{"text": "Maria walked through the quarterly revenue numbers.",
          "status": "proposed", "data": {"heading": "Quarterly Revenue Review", "start_s": 5.0},
          "evidence": ["sg_1"]}]}})");
    const Json ops = build_topic_backfill_ops(state, {});
    REQUIRE(ops.size() == 1);
    CHECK(ops[0]["op"] == "set_topic");
    CHECK(ops[0]["text"] == "Quarterly Revenue Review");
}

// from tests/test_meeting_repair.py::test_build_timeline_ops_from_live_notes_when_key_points_empty
TEST_CASE("test_build_timeline_ops_from_live_notes_when_key_points_empty",
          "[test_meeting_repair]") {
    const Json state = Json::parse(R"({"cards": {"key_points": [], "timeline": [], "live_notes": [
        {"text": "Introduced new deployment strategy.", "status": "proposed",
         "data": {"heading": "Deployment overview", "start_s": 10.0}, "evidence": ["sg_1"]},
        {"text": "Discussed rollbacks and canary releases.", "status": "proposed",
         "data": {"heading": "Canary & Rollback", "start_s": 60.0}, "evidence": ["sg_2"]}]}})");
    const Json ops = build_timeline_backfill_ops(state, rows(R"([
        {"id": "sg_1", "start_s": 10.0, "text": "Deployment strategy"},
        {"id": "sg_2", "start_s": 60.0, "text": "Canary testing"}])"));
    REQUIRE(ops.size() >= 2);
    std::vector<double> starts;
    std::string texts;
    for (const auto& op : ops) {
        if (op["data"]["start_s"].get<double>() >= 10.0) {
            starts.push_back(op["data"]["start_s"].get<double>());
        }
        texts += op["text"].get<std::string>() + " ";
    }
    CHECK(starts == std::vector<double>{10.0, 60.0});
    CHECK(texts.find("Deployment") != std::string::npos);
    CHECK(texts.find("Canary") != std::string::npos);
}

// from
// tests/test_meeting_repair.py::test_notes_can_backfill_navigation_without_becoming_duplicate_captures
TEST_CASE("test_notes_can_backfill_navigation_without_becoming_duplicate_captures",
          "[test_meeting_repair]") {
    Fixture f(state_for("m_notes"));
    f.apply("agent", "agent", R"([{"op": "add_item", "card": "live_notes",
        "text": "Database read replicas reduced latency by forty percent.",
        "data": {"heading": "Database latency", "start_s": 30.0}, "evidence": ["sg_3"]}])");
    repair_meeting_state(f.store, rows(R"([{"id": "sg_3", "start_s": 30.0,
        "text": "Read replicas cut latency by forty percent."}])"));
    const Json snapshot = f.store.snapshot();
    CHECK(snapshot["cards"]["key_points"] == Json::array());
    CHECK(snapshot["cards"]["timeline"].size() == 1);
    CHECK(contains(snapshot["rolling_summary"], "forty percent"));
}

// from tests/test_meeting_repair.py::test_notes_without_evidence_do_not_borrow_an_unrelated_segment
TEST_CASE("test_notes_without_evidence_do_not_borrow_an_unrelated_segment",
          "[test_meeting_repair]") {
    const Json state = Json::parse(
        R"({"cards": {"live_notes": [{"text": "Deployment is delayed", "data": {"start_s": 20.0}}]}})");
    CHECK(build_timeline_backfill_ops(
              state, rows(R"([{"id": "sg_1", "start_s": 1.0, "text": "Hello"}])")) ==
          Json::array());
}

TEST_CASE("repair: clipped beats, a synthesized repair item and a refused note start",
          "[meeting_state_repair]") {
    const std::string long_text(200, 'w');
    Json state = Json::parse(R"({"cards": {"key_points": [
        {"text": "x", "status": "proposed", "author_type": "system", "author_id": "state_repair",
         "data": {"insight_synthesized": true}, "evidence": ["sg_1"]}], "timeline": []}})");
    state["cards"]["key_points"][0]["text"] = long_text + "\nmore";
    const Json ops = build_timeline_backfill_ops(
        state, rows(R"([{"id": "sg_1", "start_s": "7.5", "text": "x"}])"));
    REQUIRE(ops.size() == 1);
    CHECK(ops[0]["text"] == std::string(177, 'w') + "...");
    CHECK(ops[0]["data"]["start_s"] == 7.5);
    const Json bad = Json::parse(R"({"cards": {"live_notes": [
        {"text": "Note", "data": {"start_s": "soon"}, "evidence": ["sg_1"]}]}})");
    CHECK_THROWS(build_timeline_backfill_ops(bad, rows(R"([{"id": "sg_1", "start_s": 1}])")));
}
