// tests/test_meeting_notes_agent.py: the AI note taker's live_notes card and its op gating. The
// prompts, scheduler and tool policy are M8's; export and re-decode tests are M7.11's and
// M7.14's.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <string_view>

#include "base/testing/fake_clock.h"
#include "meeting_state/patches.h"
#include "meeting_state/schema.h"
#include "meeting_state/testing/fixed_ids.h"

using fv::Json;
using namespace fv::meeting_state;

namespace {

OpContext agent_ctx(const fv::Clock& clock, IdSource& ids) {
    OpContext ctx;
    ctx.actor_type = "agent";
    ctx.actor_id = "note-taker";
    ctx.segment_exists = [](std::string_view id) {
        return id.starts_with("sg_");
    };
    ctx.clock = &clock;
    ctx.ids = &ids;
    return ctx;
}

std::vector<std::string> names(const Json& ops) {
    std::vector<std::string> out;
    for (const auto& op : ops) {
        out.push_back(op["op"].get<std::string>());
    }
    return out;
}

} // namespace

// from tests/test_meeting_notes_agent.py::TestNotesState::test_live_notes_is_a_card_and_round_trips
TEST_CASE("TestNotesState::test_live_notes_is_a_card_and_round_trips",
          "[test_meeting_notes_agent]") {
    CHECK(std::find(CARD_KEYS.begin(), CARD_KEYS.end(), "live_notes") != CARD_KEYS.end());

    MeetingState state;
    state.meeting_id = "m_notes";
    CardItem item;
    item.id = "it_n1";
    item.card = "live_notes";
    item.text = "Maria walked the funnel.";
    item.data = {{"heading", "Funnel review"}, {"start_s", 41.0}};
    state.cards["live_notes"].push_back(item);

    const MeetingState restored = MeetingState::from_json(state.to_json());
    CHECK(restored.cards["live_notes"].at(0).data["heading"] == "Funnel review");
}

// from
// tests/test_meeting_notes_agent.py::TestNotesState::test_legacy_snapshots_without_live_notes_load
TEST_CASE("TestNotesState::test_legacy_snapshots_without_live_notes_load",
          "[test_meeting_notes_agent]") {
    MeetingState state;
    state.meeting_id = "m_old";
    Json doc = state.to_json();
    doc["cards"].erase("live_notes");

    const MeetingState restored = MeetingState::from_json(doc);
    CHECK(restored.cards["live_notes"].empty());
}

// from
// tests/test_meeting_notes_agent.py::TestNotesPatchOps::test_agent_writes_notes_with_protection_flow
TEST_CASE("TestNotesPatchOps::test_agent_writes_notes_with_protection_flow",
          "[test_meeting_notes_agent]") {
    fv::FakeClock clock;
    testing::FixedIds ids;
    MeetingState state;
    state.meeting_id = "m_notes";
    const OpContext ctx = agent_ctx(clock, ids);

    const auto add = apply_ops(
        state, ctx,
        Json::array({{{"op", "add_item"},
                      {"card", "live_notes"},
                      {"text", "Maria walked the funnel: 40 percent signup to activation, down 5 "
                               "points."},
                      {"data", {{"heading", "Funnel review"}, {"start_s", 41.0}}},
                      {"evidence", {"sg_1"}}}}));
    REQUIRE(add.at(0).ok);
    const CardItem& item = state.cards["live_notes"].at(0);
    CHECK(item.status == "proposed");
    CHECK(item.data["start_s"] == 41.0);

    const auto dup = apply_ops(
        state, ctx,
        Json::array({{{"op", "add_item"},
                      {"card", "live_notes"},
                      {"text", "Maria walked the funnel: 40 percent signup to activation, down 5 "
                               "points!"},
                      {"evidence", {"sg_1"}}}}));
    CHECK(dup.at(0).reason == "duplicate_item");

    const auto update = apply_ops(
        state, ctx,
        Json::array({{{"op", "update_item"},
                      {"id", item.id},
                      {"base_revision", item.revision},
                      {"set",
                       {{"text", "Maria walked the funnel; activation down 5 points to 40 "
                                 "percent."}}},
                      {"evidence", {"sg_2"}}}}));
    CHECK(update.at(0).ok);

    // A human edit makes the block authoritative; the agent is locked out.
    OpContext human;
    human.actor_type = "user";
    human.actor_id = "p_me";
    human.clock = &clock;
    apply_ops(
        state, human,
        Json::array(
            {{{"op", "update_item"}, {"id", item.id}, {"set", {{"text", "human wording"}}}}}));
    const auto locked = apply_ops(state, ctx,
                                  Json::array({{{"op", "update_item"},
                                                {"id", item.id},
                                                {"base_revision", item.revision + 1},
                                                {"set", {{"text", "agent rewrite attempt"}}},
                                                {"evidence", {"sg_3"}}}}));
    CHECK(locked.at(0).reason == "human_edited");
}

// from tests/test_meeting_notes_agent.py::TestNotesPatchOps::test_human_can_add_a_note_directly
TEST_CASE("TestNotesPatchOps::test_human_can_add_a_note_directly", "[test_meeting_notes_agent]") {
    MeetingState state;
    state.meeting_id = "m_notes";
    OpContext guest;
    guest.actor_type = "user";
    guest.actor_id = "p_guest";
    const auto results =
        apply_ops(state, guest,
                  Json::array({{{"op", "add_item"},
                                {"card", "live_notes"},
                                {"text", "Reminder: send the deck after the call."},
                                {"data", {{"heading", "Follow-up"}}}}}));
    CHECK(results.at(0).ok);
    CHECK(state.cards["live_notes"].at(0).status == "edited");
}

// from
// tests/test_meeting_notes_agent.py::TestSharedNotesFilter::test_filter_keeps_only_live_notes_ops
TEST_CASE("TestSharedNotesFilter::test_filter_keeps_only_live_notes_ops",
          "[test_meeting_notes_agent]") {
    const Json ops = Json::array({
        {{"op", "add_item"}, {"card", "live_notes"}, {"text", "keep"}},
        {{"op", "add_item"}, {"card", "key_points"}, {"text", "drop"}},
        {{"op", "update_item"}, {"id", "it_note1"}, {"set", {{"text", "keep"}}}},
        {{"op", "update_item"}, {"id", "it_key1"}, {"set", {{"text", "drop"}}}},
        {{"op", "remove_item"}, {"id", "it_note2"}},
        {{"op", "set_topic"}, {"text", "drop"}},
        {{"op", "revise_segment_text"}, {"segment_id", "sg_1"}, {"text", "drop"}},
    });
    const Json kept =
        filter_notes_ops(ops, std::set<std::string, std::less<>>{"it_note1", "it_note2"});
    CHECK(names(kept) == std::vector<std::string>{"add_item", "update_item", "remove_item"});
    CHECK(kept[0]["card"] == "live_notes");
}

// from
// tests/test_meeting_notes_agent.py::TestSharedNotesFilter::test_filter_with_no_known_ids_allows_only_adds
TEST_CASE("TestSharedNotesFilter::test_filter_with_no_known_ids_allows_only_adds",
          "[test_meeting_notes_agent]") {
    const Json ops = Json::array({
        {{"op", "add_item"}, {"card", "live_notes"}, {"text", "keep"}},
        {{"op", "update_item"}, {"id", "it_note1"}, {"set", {{"text", "drop"}}}},
    });
    CHECK(names(filter_notes_ops(ops, std::nullopt)) == std::vector<std::string>{"add_item"});
}

// from
// tests/test_meeting_notes_agent.py::TestSharedNotesFilter::test_live_note_ids_reads_the_snapshot
TEST_CASE("TestSharedNotesFilter::test_live_note_ids_reads_the_snapshot",
          "[test_meeting_notes_agent]") {
    const Json state = {{"cards",
                         {{"live_notes",
                           {{{"id", "it_a"}, {"status", "proposed"}},
                            {{"id", "it_b"}, {"status", "removed"}},
                            {{"id", "it_c"}, {"status", "edited"}}}}}}};
    CHECK(live_note_ids(state) == std::set<std::string, std::less<>>{"it_a", "it_b", "it_c"});
    CHECK(live_note_ids(Json::object()).empty());
    CHECK(live_note_ids(Json{{"cards", Json::object()}}).empty());
}
