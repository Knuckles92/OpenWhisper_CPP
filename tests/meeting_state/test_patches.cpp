// meeting/state/patches.py: every rejection reason at least once, the gate order, the op
// registry and its domains. The golden scripts (test_golden.cpp) pin the full results.

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include "base/testing/fake_clock.h"
#include "meeting_state/patches.h"
#include "meeting_state/schema.h"
#include "meeting_state/testing/fixed_ids.h"
#include "pyport/errors.h"

using fv::Json;
using namespace fv::meeting_state;

namespace {

struct Fixture {
    fv::FakeClock clock;
    testing::FixedIds ids;
    MeetingState state;

    Fixture() {
        state.meeting_id = "m_test";
        Participant me{"p_me", "Me", "me", "human", false, "", ""};
        state.participants.set("p_me", me);
    }

    OpContext ctx(const std::string& actor_type) {
        OpContext c;
        c.actor_type = actor_type;
        c.actor_id = actor_type == "agent" ? "agent" : "p_me";
        c.segment_exists = [](std::string_view id) {
            return id.starts_with("sg_");
        };
        c.clock = &clock;
        c.ids = &ids;
        return c;
    }

    OpResult apply(const std::string& actor_type, Json op) {
        return apply_ops(state, ctx(actor_type), Json::array({std::move(op)})).at(0);
    }

    std::string reason(const std::string& actor_type, Json op) {
        const OpResult result = apply(actor_type, std::move(op));
        return result.ok ? "ok" : result.reason.value_or("");
    }

    std::string add(const std::string& actor_type, const std::string& card,
                    const std::string& text) {
        Json op = {{"op", "add_item"}, {"card", card}, {"text", text}, {"evidence", {"sg_1"}}};
        if (card == "timeline") {
            op["data"] = {{"start_s", 1}};
        }
        const OpResult result = apply(actor_type, op);
        REQUIRE(result.ok);
        return *result.target_id;
    }
};

Json ev(Json op) {
    op["evidence"] = {"sg_1"};
    return op;
}

} // namespace

TEST_CASE("apply_ops rejects at each gate in Python's order", "[patches]") {
    Fixture f;
    const auto results =
        apply_ops(f.state, f.ctx("agent"),
                  Json::array({"text",
                               {{"card", "x"}},
                               {{"op", "nope"}},
                               {{"op", "set_title"}, {"text", "T"}},
                               {{"op", "set_topic"}, {"text", "T"}},
                               {{"op", "set_topic"}, {"text", "T"}, {"evidence", "sg_1"}},
                               {{"op", "set_topic"}, {"text", "T"}, {"evidence", {"x_1"}}}}));
    REQUIRE(results.size() == 7);
    CHECK(results[0].reason == "malformed_op");
    CHECK(results[0].op == Json{{"op", "text"}});
    CHECK(results[1].reason == "malformed_op");
    CHECK(results[2].reason == "unknown_op");
    CHECK(results[3].reason == "agent_forbidden");
    CHECK(results[4].reason == "missing_evidence");
    CHECK(results[5].reason == "invalid_evidence");
    CHECK(results[6].reason == "unknown_evidence");
    for (const auto& result : results) {
        CHECK_FALSE(result.seq.has_value());
    }

    CHECK(
        f.reason("user", {{"op", "revise_segment_text"}, {"segment_id", "sg_1"}, {"text", "x"}}) ==
        "agent_only");
    CHECK(f.reason("user", {{"op", "set_rolling_summary"}, {"text", "x"}}) == "host_only");
    CHECK_THROWS_AS(apply_ops(f.state, f.ctx("host"), Json::object()), fv::py::TypeError);
}

TEST_CASE("item rejection reasons", "[patches]") {
    Fixture f;
    CHECK(f.reason("host", {{"op", "add_item"}, {"card", "bogus"}, {"text", "x"}}) ==
          "unknown_card");
    CHECK(f.reason("agent", ev({{"op", "add_item"}, {"card", "user_notes"}, {"text", "x"}})) ==
          "human_only_card");
    CHECK(f.reason("host", {{"op", "add_item"}, {"card", "risks"}, {"text", " "}}) ==
          "invalid_text");
    CHECK(f.reason("host",
                   {{"op", "add_item"}, {"card", "risks"}, {"text", std::string(4001, 'x')}}) ==
          "text_too_long");
    CHECK(f.reason("host", {{"op", "add_item"}, {"card", "risks"}, {"text", "x"}, {"data", {1}}}) ==
          "invalid_data");
    CHECK(f.reason("host", {{"op", "add_item"},
                            {"card", "risks"},
                            {"text", "x"},
                            {"data", {{"blob", std::string(5000, 'x')}}}}) == "data_too_large");
    CHECK(f.reason("agent", ev({{"op", "add_item"}, {"card", "timeline"}, {"text", "x"}})) ==
          "missing_start_s");
    CHECK(f.reason("agent", ev({{"op", "add_item"},
                                {"card", "timeline"},
                                {"text", "x"},
                                {"data", {{"start_s", "later"}}}})) == "invalid_start_s");

    const std::string id = f.add("agent", "key_points", "Budget approved for launch");
    CHECK(f.reason("agent", ev({{"op", "add_item"},
                                {"card", "key_points"},
                                {"text", "budget approved for LAUNCH"}})) == "duplicate_item");
    CHECK(f.reason("agent", ev({{"op", "update_item"}, {"id", "it_none"}})) == "unknown_item");
    CHECK(f.reason("agent", ev({{"op", "update_item"},
                                {"id", id},
                                {"base_revision", 9},
                                {"set", {{"text", "y"}}}})) == "revision_mismatch");
    CHECK(f.apply("agent", ev({{"op", "update_item"}, {"id", id}, {"set", {{"text", "y"}}}}))
              .current_revision == 1);
    CHECK(f.reason("host", {{"op", "update_item"}, {"id", id}, {"set", Json::object()}}) ==
          "empty_update");
    CHECK(f.reason("host", {{"op", "update_item"}, {"id", id}, {"set", {{"text", "Human"}}}}) ==
          "ok");
    CHECK(f.reason("agent", ev({{"op", "update_item"},
                                {"id", id},
                                {"base_revision", 2},
                                {"set", {{"text", "y"}}}})) == "human_edited");
    CHECK(f.reason("host", {{"op", "remove_item"}, {"id", id}}) == "ok");
    CHECK(f.reason("host", {{"op", "remove_item"}, {"id", id}}) == "already_removed");

    Fixture full;
    for (std::size_t n = 0; n < MAX_CARD_ITEMS; ++n) {
        CardItem item;
        item.id = "it_" + std::to_string(n);
        item.card = "risks";
        full.state.cards["risks"].push_back(item);
    }
    CHECK(full.reason("host", {{"op", "add_item"}, {"card", "risks"}, {"text", "x"}}) ==
          "card_full");
}

TEST_CASE("participant and question rejection reasons", "[patches]") {
    Fixture f;
    CHECK(f.reason("agent",
                   ev({{"op", "upsert_participant"}, {"display_name", "X"}, {"kind", "guest"}})) ==
          "invalid_kind");
    CHECK(f.reason("host", {{"op", "upsert_participant"}, {"id", "p_x"}, {"display_name", "X"}}) ==
          "unknown_participant");
    CHECK(f.reason("agent", ev({{"op", "suggest_participant_name"},
                                {"participant_id", "p_me"},
                                {"display_name", "X"}})) == "human_named");
    CHECK(f.reason("agent", ev({{"op", "rename_participant"},
                                {"participant_id", "p_me"},
                                {"display_name", "X"}})) == "agent_forbidden");

    Fixture crowded;
    for (int n = 1; n < static_cast<int>(MAX_PARTICIPANTS); ++n) {
        crowded.state.participants.set("p_" + std::to_string(n), Participant{});
    }
    CHECK(crowded.reason("host", {{"op", "upsert_participant"}, {"display_name", "X"}}) ==
          "participant_limit");

    for (std::size_t n = 0; n < MAX_OPEN_QUESTIONS; ++n) {
        CHECK(f.reason("agent", ev({{"op", "ask_question"}, {"text", "Q" + std::to_string(n)}})) ==
              "ok");
    }
    CHECK(f.reason("agent", ev({{"op", "ask_question"}, {"text", "Too many"}})) ==
          "question_limit");
    const std::string first = "q_000000000001";
    CHECK(f.reason("agent", ev({{"op", "resolve_question"},
                                {"question_id", "q_x"},
                                {"answer_text", "a"},
                                {"confidence", 1}})) == "unknown_question");
    CHECK(f.reason("agent", ev({{"op", "resolve_question"},
                                {"question_id", first},
                                {"answer_text", "a"},
                                {"confidence", "high"}})) == "invalid_confidence");
    CHECK(f.reason("agent", ev({{"op", "resolve_question"},
                                {"question_id", first},
                                {"answer_text", "a"},
                                {"confidence", 0.39}})) == "low_confidence");
    CHECK(f.reason("agent", ev({{"op", "resolve_question"},
                                {"question_id", first},
                                {"answer_text", "a"},
                                {"confidence", 0.8}})) == "ok");
    CHECK(f.reason("agent", ev({{"op", "resolve_question"},
                                {"question_id", first},
                                {"answer_text", "a"},
                                {"confidence", 0.9}})) == "already_closed");
    CHECK(f.reason("host", {{"op", "reopen_question"}, {"question_id", first}}) == "ok");
    CHECK(f.reason("host", {{"op", "reopen_question"}, {"question_id", first}}) == "already_open");
}

TEST_CASE("segment op rejection reasons", "[patches]") {
    Fixture f;
    CHECK(f.reason("host", {{"op", "reassign_segment_speaker"}, {"segment_id", ""}}) ==
          "invalid_segment");
    CHECK(f.reason("host", {{"op", "reassign_segment_speaker"}, {"segment_id", "x_1"}}) ==
          "unknown_segment");

    OpContext system = f.ctx("system");
    system.segment_pinned = [](std::string_view) -> bool {
        throw std::runtime_error("db down");
    };
    const auto pinned =
        apply_ops(f.state, system,
                  Json::array({{{"op", "reassign_segment_speaker"}, {"segment_id", "sg_1"}}}));
    CHECK(pinned.at(0).reason == "segment_pinned");

    OpResult note = f.apply("host", {{"op", "add_item"},
                                     {"card", "user_notes"},
                                     {"text", "fix"},
                                     {"evidence", {"sg_2"}},
                                     {"data",
                                      {{"kind", "occurrence_correction"},
                                       {"selected_text", "a"},
                                       {"replacement", "b"},
                                       {"occurrence_index", 0},
                                       {"base_fingerprint", "fnv1a64:0000000000000000"}}}});
    REQUIRE(note.ok);
    CHECK(f.reason("agent", {{"op", "revise_segment_text"},
                             {"segment_id", "sg_2"},
                             {"text", "x"},
                             {"evidence", {"sg_2"}}}) == "human_scoped_correction");
    CHECK(f.reason("agent", {{"op", "revise_segment_text"},
                             {"segment_id", "sg_3"},
                             {"text", "x"},
                             {"evidence", {"sg_1"}}}) == "missing_evidence");
}

TEST_CASE("a handler that throws is internal_error and the batch goes on", "[patches]") {
    Fixture f;
    const std::string id = f.add("host", "key_points", "Reviewed point");
    // item_effect reads q["item_id"]: a malformed review question raises, as in Python.
    f.state.insight_review = {{"questions", {{{"id", "rq"}}}}};
    const auto results =
        apply_ops(f.state, f.ctx("host"),
                  Json::array({{{"op", "update_item"}, {"id", id}, {"set", {{"text", "Edited"}}}},
                               {{"op", "set_cloud_enabled"}, {"enabled", true}}}));
    CHECK(results.at(0).reason == "internal_error");
    CHECK(results.at(1).ok);
    CHECK(f.state.cloud_enabled);
}

TEST_CASE("every op has domains and flags matching Python's sets", "[patches]") {
    const auto& domains = op_domains();
    const std::set<std::string, std::less<>> builtins = {"add_item",
                                                         "update_item",
                                                         "remove_item",
                                                         "pin_item",
                                                         "unpin_item",
                                                         "confirm_item",
                                                         "set_topic",
                                                         "set_rolling_summary",
                                                         "set_title",
                                                         "set_meeting_intent",
                                                         "set_cloud_enabled",
                                                         "upsert_participant",
                                                         "suggest_participant_name",
                                                         "rename_participant",
                                                         "ask_question",
                                                         "resolve_question",
                                                         "answer_question",
                                                         "dismiss_question",
                                                         "reopen_question",
                                                         "reassign_segment_speaker",
                                                         "revise_segment_text"};
    for (const auto& name : builtins) {
        INFO(name);
        CHECK(domains.contains(name));
    }
    for (const auto& name : all_ops()) {
        INFO(name);
        REQUIRE(domains.contains(name));
        const OpDomains& d = domains.at(name);
        CHECK((d.cards || d.questions || d.participants || d.topic || d.summary || d.title ||
               d.intent || d.cloud || d.review || d.reports || d.highlights || d.feedback ||
               d.segments));
    }
    for (const auto name : AGENT_OPS) {
        CHECK(op_flags(name)->agent);
    }
    for (const auto name : AGENT_ONLY_OPS) {
        CHECK(op_flags(name)->agent_only);
    }
    for (const auto name : HOST_ONLY_OPS) {
        CHECK(op_flags(name)->host_only);
    }
    for (const auto name : SEGMENT_OPS) {
        CHECK(op_flags(name)->segment);
    }
    CHECK_FALSE(op_flags("pin_item")->agent);
    CHECK_FALSE(op_flags("nope").has_value());
    // The locked single lookup agrees with the map.
    for (const auto& name : all_ops()) {
        INFO(name);
        CHECK(op_domain(name) == domains.at(name));
    }
    CHECK(op_domain("add_item")->cards);
    CHECK(op_domain("reassign_segment_speaker")->segments);
    CHECK_FALSE(op_domain("nope").has_value());
}

TEST_CASE("a later package registers an op without editing the dispatch", "[patches]") {
    register_op(
        "test_mark_feedback",
        [](MeetingState& state, Json& op, const OpContext&) {
            state.voice_feedback = {{"text", op.value("text", "")}};
            OpResult result;
            result.ok = true;
            result.op = op;
            result.effect = {{"entity", "voice_feedback"}};
            return result;
        },
        OpDomains{.feedback = true}, OpFlags{.host_only = true});
    CHECK_THROWS_AS(register_op("test_mark_feedback", {}, OpDomains{.feedback = true}, OpFlags{}),
                    std::logic_error);
    CHECK(all_ops().contains("test_mark_feedback"));
    CHECK(op_domains().at("test_mark_feedback").feedback);

    Fixture f;
    CHECK(f.reason("user", {{"op", "test_mark_feedback"}, {"text", "hi"}}) == "host_only");
    CHECK(f.reason("agent", ev({{"op", "test_mark_feedback"}})) == "agent_forbidden");
    CHECK(f.reason("host", {{"op", "test_mark_feedback"}, {"text", "hi"}}) == "ok");
    CHECK(f.state.voice_feedback == Json{{"text", "hi"}});
}

TEST_CASE("item edits supersede linked review questions", "[patches]") {
    Fixture f;
    const std::string id = f.add("host", "key_points", "Vendor A wins");
    const std::string note = f.add("host", "user_notes", "Vendor B, not A");
    const std::string block = f.add("host", "live_notes",
                                    "Notes\n\nUser clarification (supersedes earlier wording on "
                                    "this point): Vendor B, not A");
    f.state.insight_review = {
        {"questions", {{{"id", "rq_1"}, {"item_id", id}, {"note_id", note}}}}};

    const OpResult result =
        f.apply("host", {{"op", "update_item"}, {"id", id}, {"set", {{"text", "Vendor B"}}}});

    REQUIRE(result.ok);
    CHECK(result.effect["entity"] == "review");
    CHECK(result.effect["review"]["questions"][0]["superseded"] == true);
    CHECK(result.effect["items"].size() == 3);
    CHECK(f.state.find_item(note)->text ==
          "Earlier review, superseded by a later insight edit: Vendor B, not A");
    CHECK(f.state.find_item(block)->text ==
          "Notes\n\nEarlier clarification, superseded by a later insight edit: Vendor B, not A");
    // Without related questions the effect is the item alone.
    const OpResult again =
        f.apply("host", {{"op", "update_item"}, {"id", id}, {"set", {{"text", "Vendor C"}}}});
    CHECK(again.effect["entity"] == "item");
}

TEST_CASE("evidence ids that are not text are refused without a segment predicate", "[patches]") {
    // Python stores them as given; ids are text here (a documented difference).
    Fixture f;
    OpContext host = f.ctx("host");
    host.segment_exists = {};
    const auto results = apply_ops(
        f.state, host,
        Json::array({{{"op", "add_item"}, {"card", "risks"}, {"text", "x"}, {"evidence", {5}}}}));
    CHECK(results.at(0).reason == "invalid_evidence");
}
