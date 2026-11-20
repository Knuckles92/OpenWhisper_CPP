// tests/test_meeting_intent.py: the host's meeting brief as a state op and in snapshots. Python
// runs the op tests through MeetingStateStore; the store is M7.7's, so they run through
// apply_ops, which is what the store calls. Undo is M7.7's, the prompts M8's, the export M7.11's.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "base/testing/fake_clock.h"
#include "meeting_state/patches.h"
#include "meeting_state/schema.h"
#include "meeting_state/testing/fixed_ids.h"

using fv::Json;
using namespace fv::meeting_state;

namespace {

const std::string kBrief = "We decide the vendor today. Capture who objected and why, and the "
                           "number attached to each bid.";

struct Fixture {
    fv::FakeClock clock;
    testing::FixedIds ids;
    MeetingState state;

    Fixture() { state.meeting_id = "m_intent"; }

    OpResult apply(const std::string& actor_type, const std::string& actor_id, Json op) {
        OpContext ctx;
        ctx.actor_type = actor_type;
        ctx.actor_id = actor_id;
        ctx.clock = &clock;
        ctx.ids = &ids;
        return apply_ops(state, ctx, Json::array({std::move(op)})).at(0);
    }
};

} // namespace

// from tests/test_meeting_intent.py::TestIntentOp::test_host_writes_the_brief
TEST_CASE("TestIntentOp::test_host_writes_the_brief", "[test_meeting_intent]") {
    Fixture f;
    const OpResult result =
        f.apply("host", "p_host", {{"op", "set_meeting_intent"}, {"text", kBrief}});

    CHECK(result.ok);
    CHECK(result.effect["entity"] == "intent");
    CHECK(result.effect["intent"]["text"] == kBrief);
    const Json snapshot = f.state.to_json()["intent"];
    CHECK(snapshot["text"] == kBrief);
    CHECK(snapshot["author_id"] == "p_host");
    CHECK_FALSE(snapshot["updated_at"].get<std::string>().empty());
}

// from
// tests/test_meeting_intent.py::TestIntentOp::test_guests_cannot_redirect_what_the_meeting_captures
TEST_CASE("TestIntentOp::test_guests_cannot_redirect_what_the_meeting_captures",
          "[test_meeting_intent]") {
    Fixture f;
    const OpResult result =
        f.apply("user", "p_guest", {{"op", "set_meeting_intent"}, {"text", "Only my bits"}});

    CHECK_FALSE(result.ok);
    CHECK(result.reason == "host_only");
    CHECK(f.state.to_json()["intent"]["text"] == "");
}

// from tests/test_meeting_intent.py::TestIntentOp::test_the_agent_cannot_write_its_own_brief
TEST_CASE("TestIntentOp::test_the_agent_cannot_write_its_own_brief", "[test_meeting_intent]") {
    Fixture f;
    const OpResult result = f.apply(
        "agent", "agent",
        {{"op", "set_meeting_intent"}, {"text", "Capture everything"}, {"evidence", {"sg_1"}}});

    CHECK_FALSE(result.ok);
    CHECK(result.reason == "agent_forbidden");
}

// from
// tests/test_meeting_intent.py::TestIntentOp::test_an_oversized_brief_is_rejected_rather_than_truncated
TEST_CASE("TestIntentOp::test_an_oversized_brief_is_rejected_rather_than_truncated",
          "[test_meeting_intent]") {
    Fixture f;
    const OpResult result =
        f.apply("host", "p_host",
                {{"op", "set_meeting_intent"}, {"text", std::string(MAX_INTENT_LEN + 1, 'x')}});

    CHECK_FALSE(result.ok);
    CHECK(result.reason == "invalid_text");
    CHECK(f.state.to_json()["intent"]["text"] == "");
}

// from tests/test_meeting_intent.py::TestIntentOp::test_blank_text_clears_the_brief
TEST_CASE("TestIntentOp::test_blank_text_clears_the_brief", "[test_meeting_intent]") {
    Fixture f;
    f.apply("host", "p_host", {{"op", "set_meeting_intent"}, {"text", kBrief}});

    const OpResult result =
        f.apply("host", "p_host", {{"op", "set_meeting_intent"}, {"text", "  "}});

    CHECK(result.ok);
    CHECK(f.state.to_json()["intent"]["text"] == "");
    // The inverse restores the brief that was cleared.
    CHECK(result.inverse == Json{{"op", "set_meeting_intent"}, {"text", kBrief}});
}

// from
// tests/test_meeting_intent.py::TestIntentPersistence::test_the_brief_round_trips_through_a_snapshot
TEST_CASE("TestIntentPersistence::test_the_brief_round_trips_through_a_snapshot",
          "[test_meeting_intent]") {
    MeetingState original;
    original.meeting_id = "m_intent";
    original.intent.text = kBrief;
    original.intent.author_id = "p_host";

    const MeetingState restored = MeetingState::from_json(original.to_json());

    CHECK(restored.intent.text == kBrief);
    CHECK(restored.intent.author_id == "p_host");
}

// from
// tests/test_meeting_intent.py::TestIntentPersistence::test_a_snapshot_written_before_this_feature_still_loads
TEST_CASE("TestIntentPersistence::test_a_snapshot_written_before_this_feature_still_loads",
          "[test_meeting_intent]") {
    MeetingState old;
    old.meeting_id = "m_old";
    Json legacy = old.to_json();
    legacy.erase("intent");

    const MeetingState restored = MeetingState::from_json(legacy);

    CHECK(restored.intent.text.empty());
}
