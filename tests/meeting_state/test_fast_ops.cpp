// tests/test_meeting_fast_features.py: the fast state ops (meeting/fast_state.py). The judges,
// live signals, verifier and voice commands that publish them are M8's.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include "base/testing/fake_clock.h"
#include "meeting_state/fast_ops.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"
#include "meeting_state/testing/fixed_ids.h"

using fv::Json;
using namespace fv::meeting_state;

namespace {

struct Fixture {
    fv::FakeClock clock;
    testing::FixedIds ids;
    MeetingStateStore store;

    Fixture() : store(state(), nullptr, {}, clock, ids) {}

    static MeetingState state() {
        MeetingState state;
        state.meeting_id = "m_test";
        state.cloud_enabled = true;
        return state;
    }

    Json add_claim() {
        const auto result = store.apply("agent", std::string("notes"), Json::parse(R"([{"op":
            "add_item", "card": "decisions", "text": "The beta ships on Friday.",
            "evidence": ["sg_one"]}])"))[0];
        REQUIRE(result.ok);
        return result.effect["item"];
    }

    OpResult apply(const std::string& actor, const std::optional<std::string>& id, const Json& op) {
        return store.apply(actor, id, Json::array({op}))[0];
    }
};

} // namespace

// from tests/test_meeting_fast_features.py::test_annotations_cannot_be_forged_by_guest
TEST_CASE("test_annotations_cannot_be_forged_by_guest", "[test_meeting_fast_features]") {
    Fixture f;
    const Json item = f.add_claim();
    for (const Json& op : {Json{{"op", "citation_check"}, {"id", item["id"]}, {"revision", 1}},
                           Json{{"op", "publish_highlights"}, {"pulses", Json::array()}},
                           Json{{"op", "voice_feedback"}, {"message", "fake"}}}) {
        CHECK_FALSE(f.apply("user", "guest", op).ok);
    }
}

// from tests/test_meeting_fast_features.py::test_persisted_signals_round_trip
TEST_CASE("test_persisted_signals_round_trip", "[test_meeting_fast_features]") {
    Fixture f;
    const auto result = f.apply("system", "live-signals", Json::parse(R"({"op":
        "publish_highlights", "pulses": [{"id": "pulse_0_decision", "kind": "decision",
        "start_s": 10, "segment_id": "sg_one", "probability": 0.9, "text": "Ship"}]})"));
    CHECK(result.ok);
    const Json snapshot = f.store.snapshot();
    CHECK(MeetingState::from_json(snapshot, f.clock).to_json() == snapshot);
}

TEST_CASE("fast ops: pulses merge by id, a minute replaces its own, and the order is by start",
          "[meeting_state_fast]") {
    Fixture f;
    const auto first = f.apply("system", "live-signals", Json::parse(R"({"op":
        "publish_highlights", "minute": 1, "pulses": [
          {"id": "pulse_1_decision", "kind": "decision", "start_s": 70, "text": "B"},
          {"id": "pulse_1_number", "kind": "number", "start_s": 61, "text": "A"}]})"));
    REQUIRE(first.ok);
    CHECK(first.effect["entity"] == "live_highlights");
    CHECK(first.effect["pulses"][0]["id"] == "pulse_1_number");
    const auto second = f.apply("system", "live-signals", Json::parse(R"({"op":
        "publish_highlights", "minute": 1, "pulses": [
          {"id": "pulse_1_takeaway", "kind": "takeaway", "start_s": 65, "text": "C"}]})"));
    REQUIRE(second.ok);
    const Json pulses = f.store.snapshot()["live_highlights"];
    REQUIRE(pulses.size() == 1);
    CHECK(pulses[0]["id"] == "pulse_1_takeaway");
    CHECK(f.apply("system", "live-signals",
                  {{"op", "publish_highlights"}, {"minute", true}, {"pulses", Json::array()}})
              .reason == "invalid_minute");
    CHECK(f.apply("system", "live-signals",
                  {{"op", "publish_highlights"}, {"minute", -1}, {"pulses", Json::array()}})
              .reason == "invalid_minute");
    CHECK(f.apply("system", "live-signals",
                  {{"op", "publish_highlights"},
                   {"pulses", Json::parse(R"([{"id": "x", "kind": "gossip", "start_s": 1}])")}})
              .reason == "invalid_pulse");
    CHECK(
        f.apply("system", "live-signals", {{"op", "publish_highlights"}, {"pulses", "x"}}).reason ==
        "invalid_pulses");
    CHECK(f.apply("system", "live-signals",
                  {{"op", "publish_highlights"}, {"final", true}, {"pulses", Json::array()}})
              .reason == "inactive");
    CHECK(f.apply("system", "other", {{"op", "publish_highlights"}}).reason == "system_only");
}

TEST_CASE("fast ops: citation checks annotate one revision and invalidation clears them",
          "[meeting_state_fast]") {
    Fixture f;
    const Json item = f.add_claim();
    const std::string id = item["id"].get<std::string>();
    CHECK(f.apply("system", "citation-verifier",
                  {{"op", "citation_check"},
                   {"id", id},
                   {"revision", 2},
                   {"check", {{"status", "supported"}}}})
              .reason == "stale_check");
    CHECK(f.apply("system", "citation-verifier",
                  {{"op", "citation_check"}, {"id", "it_nope"}, {"revision", 1}})
              .reason == "unknown_item");
    const auto checked = f.apply("system", "citation-verifier",
                                 {{"op", "citation_check"},
                                  {"id", id},
                                  {"revision", 1.0},
                                  {"check", {{"status", "supported"}}}});
    REQUIRE(checked.ok);
    CHECK(checked.effect["item"]["citation_check"]["status"] == "supported");
    CHECK(checked.effect["item"]["revision"] == 1);
    const auto untouched = f.apply("system", "citation-verifier",
                                   {{"op", "invalidate_citations"}, {"segment_ids", {"sg_other"}}});
    REQUIRE(untouched.ok);
    CHECK(untouched.effect["items"].empty());
    const auto cleared = f.apply("system", "citation-verifier",
                                 {{"op", "invalidate_citations"}, {"segment_ids", {"sg_one"}}});
    REQUIRE(cleared.ok);
    CHECK(cleared.effect["entity"] == "review");
    CHECK(cleared.effect["items"].size() == 1);
    CHECK(f.store.snapshot()["cards"]["decisions"][0]["citation_check"] == Json::object());
}

TEST_CASE("fast ops: voice feedback while the meeting runs", "[meeting_state_fast]") {
    Fixture f;
    const auto said = f.apply("system", "voice_command",
                              {{"op", "voice_feedback"}, {"message", std::string(600, 'v')}});
    REQUIRE(said.ok);
    CHECK(said.effect["feedback"]["message"].get<std::string>().size() == 500);
    CHECK(said.effect["feedback"]["at"] == "2026-01-01T00:00:00Z");
    const auto missing = f.apply("system", "voice_command", {{"op", "voice_feedback"}});
    REQUIRE(missing.ok);
    CHECK(missing.effect["feedback"]["message"] == "");
    REQUIRE(f.store.update_runtime_fields({{"status", "ended"}}));
    CHECK(f.apply("system", "voice_command", {{"op", "voice_feedback"}, {"message", "x"}}).reason ==
          "inactive");
}
