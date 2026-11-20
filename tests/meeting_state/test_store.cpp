// meeting/state/store.py through the SQL repository: patch ops, protection rules and the
// single-writer store (seq numbering, audit events, undo). Ports tests/test_meeting_state.py,
// one test of test_meeting_intent.py and three of test_meeting_cloud_speakers.py.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "meeting_state/patches.h"
#include "meeting_state/schema.h"
#include "meeting_state/segment_ops.h"
#include "meeting_state/store.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "sql_support.h"

using fv::Json;
using namespace fv::meeting_state;
using namespace fv::meeting_state::test_support;

namespace {

Json ops(const char* text) {
    return Json::parse(text);
}

bool is_agent_op(const Json& op) {
    const Json& name = op.contains("op") ? op.at("op") : Json();
    return name.is_string() && std::find(AGENT_OPS.begin(), AGENT_OPS.end(),
                                         name.get<std::string>()) != AGENT_OPS.end();
}

// make_store(): a store over the SQL repository with one known segment, the test's segment
// handler (the prior speaker as the inverse) and the evidence predicate. apply() is
// LegacyFixtureStore's: agent ops that predate evidence enforcement cite sg_known.
struct Store {
    // `pinned`: the store also asks whether a segment carries a human speaker pin.
    explicit Store(bool pinned = false) {
        make_meeting(*sql.repo, "m_test");
        sql.repo->add_segments({make_segment("m_test", "sg_known")});
        auto repo = sql.repo;
        SegmentHandler handler = [repo](OpResult& result) -> Json {
            const std::string segment_id = result.effect.at("segment_id").get<std::string>();
            const auto prior = repo->get_segment("m_test", segment_id);
            if (!prior) {
                throw fv::py::ValueError("unknown segment");
            }
            return {{"op", "reassign_segment_speaker"},
                    {"segment_id", segment_id},
                    {"participant_id", prior->at("speaker_participant_id")}};
        };
        SegmentPredicates predicates;
        predicates.segment_exists = [repo](std::string_view id) {
            return repo->segment_exists("m_test", id);
        };
        if (pinned) {
            predicates.segment_pinned = [repo](std::string_view id) {
                const auto row = repo->get_segment("m_test", id);
                return row && row->at("speaker_pinned").get<bool>();
            };
        }
        store = std::make_shared<MeetingStateStore>(state_for("m_test"), repo, handler, sql.clock,
                                                    sql.ids, predicates);
    }
    std::vector<OpResult> apply(std::string_view actor, const std::optional<std::string>& actor_id,
                                Json batch) {
        if (actor == "agent") {
            for (auto& op : batch) {
                if (op.is_object() && is_agent_op(op) && !op.contains("evidence")) {
                    op["evidence"] = Json::array({"sg_known"});
                }
            }
        }
        return store->apply(actor, actor_id, batch);
    }
    OpResult one(std::string_view actor, const std::optional<std::string>& actor_id,
                 const char* batch) {
        auto results = apply(actor, actor_id, ops(batch));
        REQUIRE(results.size() == 1);
        return results.front();
    }
    void with_segments(std::initializer_list<const char*> ids) {
        std::vector<TranscriptSegment> segments;
        double start = 10.0;
        for (const char* id : ids) {
            segments.push_back(make_segment("m_test", id, start, start + 1.0));
            start += 2.0;
        }
        sql.repo->add_segments(segments);
    }
    Json segment(const std::string& id) { return *sql.repo->get_segment("m_test", id); }

    SqlFixture sql;
    std::shared_ptr<MeetingStateStore> store;
};

Json item_in(const Json& snapshot, const char* card, const std::string& id) {
    for (const auto& item : snapshot.at("cards").at(card)) {
        if (item.at("id") == id) {
            return item;
        }
    }
    throw std::logic_error("no item " + id);
}

} // namespace

// TestItemOps

// from
// tests/test_meeting_state.py::TestItemOps::test_minutes_restate_card_facts_but_cannot_repeat_note_blocks
TEST_CASE("TestItemOps::test_minutes_restate_card_facts_but_cannot_repeat_note_blocks",
          "[test_meeting_state]") {
    Store s;
    const std::vector<std::string> facts = {
        "Pilot budget cap is $500, not $5000.",
        "Maya will deliver the API comparison Tuesday, not Friday."};
    Json cards = Json::array();
    Json notes = Json::array();
    for (const auto& text : facts) {
        cards.push_back({{"op", "add_item"},
                         {"card", "key_points"},
                         {"text", text},
                         {"evidence", {"sg_known"}}});
        notes.push_back({{"op", "add_item"},
                         {"card", "live_notes"},
                         {"text", "The team confirmed: " + text},
                         {"evidence", {"sg_known"}}});
    }
    for (const auto& result : s.apply("agent", "agent", cards)) {
        CHECK(result.ok);
    }
    const auto noted = s.apply("agent", "agent", notes);
    for (const auto& result : noted) {
        CHECK(result.ok);
    }
    const auto duplicate = s.apply("agent", "agent", Json::array({noted[0].op}))[0];
    CHECK_FALSE(duplicate.ok);
    CHECK(duplicate.reason == "duplicate_item");
}

// from
// tests/test_meeting_state.py::TestItemOps::test_card_facts_can_be_added_after_the_note_taker_records_them
TEST_CASE("TestItemOps::test_card_facts_can_be_added_after_the_note_taker_records_them",
          "[test_meeting_state]") {
    Store s;
    Json note = {{"op", "add_item"},
                 {"card", "live_notes"},
                 {"text", "Pilot budget cap is $500."},
                 {"evidence", {"sg_known"}}};
    CHECK(s.apply("agent", "agent", Json::array({note}))[0].ok);
    note["card"] = "key_points";
    CHECK(s.apply("agent", "agent", Json::array({note}))[0].ok);
}

// from tests/test_meeting_state.py::TestItemOps::test_agent_add_item_is_proposed
TEST_CASE("TestItemOps::test_agent_add_item_is_proposed", "[test_meeting_state]") {
    Store s;
    const auto result = s.one("agent", "agent", R"([{"op": "add_item", "card": "key_points",
        "text": "Budget approved", "evidence": ["sg_known"]}])");
    CHECK(result.ok);
    CHECK(result.effect["item"]["status"] == "proposed");
    CHECK(result.effect["item"]["author_type"] == "agent");
    CHECK(result.seq == 1);
}

// from tests/test_meeting_state.py::TestItemOps::test_agent_duplicate_item_rejected
TEST_CASE("TestItemOps::test_agent_duplicate_item_rejected", "[test_meeting_state]") {
    Store s;
    s.one("agent", "agent", R"([{"op": "add_item", "card": "key_points",
        "text": "Budget approved!", "evidence": ["sg_known"]}])");
    const auto dup = s.one("agent", "agent", R"([{"op": "add_item", "card": "key_points",
        "text": "budget approved", "evidence": ["sg_known"]}])");
    CHECK_FALSE(dup.ok);
    CHECK(dup.reason == "duplicate_item");

    // Different claim: allowed.
    const auto near = s.one("agent", "agent", R"([{"op": "add_item", "card": "key_points",
        "text": "The budget was approved by the team", "evidence": ["sg_known"]}])");
    CHECK(near.ok);

    const auto paraphrase = s.one("agent", "agent", R"([{"op": "add_item", "card": "key_points",
        "text": "The speaker made a significant discovery three and a half years ago that changed their view on these topics.",
        "evidence": ["sg_known"]}])");
    CHECK(paraphrase.ok);
    const auto near_dup = s.one("agent", "agent", R"([{"op": "add_item", "card": "key_points",
        "text": "The speaker's significant discovery from three and a half years ago changed their perspective on these topics.",
        "evidence": ["sg_known"]}])");
    CHECK_FALSE(near_dup.ok);
    CHECK(near_dup.reason == "duplicate_item");
}

// from tests/test_meeting_state.py::TestItemOps::test_agent_cross_card_duplicate_item_rejected
TEST_CASE("TestItemOps::test_agent_cross_card_duplicate_item_rejected", "[test_meeting_state]") {
    Store s;
    s.one("agent", "agent", R"([{"op": "add_item", "card": "key_points",
        "text": "Schedule follow up sync with team by Friday", "evidence": ["sg_known"]}])");
    const auto cross_dup = s.one("agent", "agent", R"([{"op": "add_item", "card": "action_items",
        "text": "Schedule follow up sync with team by Friday", "evidence": ["sg_known"]}])");
    CHECK_FALSE(cross_dup.ok);
    CHECK(cross_dup.reason == "duplicate_item");

    const auto cross_decision_dup = s.one("agent", "agent", R"([{"op": "add_item",
        "card": "decisions", "text": "Schedule follow-up sync with the team by Friday",
        "evidence": ["sg_known"]}])");
    CHECK_FALSE(cross_decision_dup.ok);
    CHECK(cross_decision_dup.reason == "duplicate_item");
}

// from tests/test_meeting_state.py::TestItemOps::test_agent_timeline_requires_start_s
TEST_CASE("TestItemOps::test_agent_timeline_requires_start_s", "[test_meeting_state]") {
    Store s;
    const auto missing = s.one("agent", "agent", R"([{"op": "add_item", "card": "timeline",
        "text": "Kickoff", "evidence": ["sg_known"]}])");
    CHECK_FALSE(missing.ok);
    CHECK(missing.reason == "missing_start_s");

    const auto ok = s.one("agent", "agent", R"([{"op": "add_item", "card": "timeline",
        "text": "Kickoff", "data": {"start_s": 12.5}, "evidence": ["sg_known"]}])");
    CHECK(ok.ok);
    CHECK(ok.effect["item"]["data"]["start_s"] == 12.5);
}

// from tests/test_meeting_state.py::TestItemOps::test_human_add_item_is_edited
TEST_CASE("TestItemOps::test_human_add_item_is_edited", "[test_meeting_state]") {
    Store s;
    const auto result = s.one("user", "p_guest1",
                              R"([{"op": "add_item", "card": "user_notes", "text": "My note"}])");
    CHECK(result.ok);
    CHECK(result.effect["item"]["status"] == "edited");
    CHECK(result.effect["item"]["author_id"] == "p_guest1");
}

// from tests/test_meeting_state.py::TestItemOps::test_agent_update_requires_matching_revision
TEST_CASE("TestItemOps::test_agent_update_requires_matching_revision", "[test_meeting_state]") {
    Store s;
    const auto added =
        s.one("agent", "agent", R"([{"op": "add_item", "card": "decisions", "text": "Ship v1"}])");
    const std::string item_id = *added.target_id;

    const auto wrong = s.apply("agent", "agent",
                               Json::array({{{"op", "update_item"},
                                             {"id", item_id},
                                             {"base_revision", 99},
                                             {"set", {{"text", "Ship v2"}}}}}))[0];
    CHECK_FALSE(wrong.ok);
    CHECK(wrong.reason == "revision_mismatch");
    CHECK(wrong.current_revision == 1);

    const auto right = s.apply("agent", "agent",
                               Json::array({{{"op", "update_item"},
                                             {"id", item_id},
                                             {"base_revision", 1},
                                             {"set", {{"text", "Ship v2"}}}}}))[0];
    CHECK(right.ok);
    CHECK(right.effect["item"]["revision"] == 2);
}

// from
// tests/test_meeting_state.py::TestItemOps::test_live_notes_update_preserves_heading_and_start_s
TEST_CASE("TestItemOps::test_live_notes_update_preserves_heading_and_start_s",
          "[test_meeting_state]") {
    Store s;
    const auto added = s.one("agent", "agent", R"([{"op": "add_item", "card": "live_notes",
        "text": "The kickoff covered the roadmap.",
        "data": {"heading": "Kickoff", "start_s": 12.0}}])");
    // A partial data dict (only start_s re-sent) must not drop the heading.
    const auto updated =
        s.apply("agent", "agent",
                Json::array({{{"op", "update_item"},
                              {"id", *added.target_id},
                              {"base_revision", 1},
                              {"set",
                               {{"text", "The kickoff covered the roadmap and staffing."},
                                {"data", {{"start_s", 12.0}}}}}}}))[0];
    CHECK(updated.ok);
    const Json& data = updated.effect["item"]["data"];
    CHECK(data["heading"] == "Kickoff");
    CHECK(data["start_s"] == 12.0);
    CHECK(updated.effect["item"]["text"] == "The kickoff covered the roadmap and staffing.");
}

// from tests/test_meeting_state.py::TestItemOps::test_human_edit_protects_item_from_agent
TEST_CASE("TestItemOps::test_human_edit_protects_item_from_agent", "[test_meeting_state]") {
    Store s;
    const auto added = s.one(
        "agent", "agent", R"([{"op": "add_item", "card": "action_items", "text": "Send report"}])");
    const std::string item_id = *added.target_id;

    const auto edited = s.apply("user", "p_1",
                                Json::array({{{"op", "update_item"},
                                              {"id", item_id},
                                              {"set", {{"text", "Send report by Friday"}}}}}))[0];
    CHECK(edited.ok);
    CHECK(edited.effect["item"]["status"] == "edited");

    const auto agent_try = s.apply("agent", "agent",
                                   Json::array({{{"op", "update_item"},
                                                 {"id", item_id},
                                                 {"base_revision", 2},
                                                 {"set", {{"text", "changed"}}}}}))[0];
    CHECK_FALSE(agent_try.ok);
    CHECK(agent_try.reason == "human_edited");

    const auto agent_remove =
        s.apply("agent", "agent",
                Json::array({{{"op", "remove_item"}, {"id", item_id}, {"base_revision", 2}}}))[0];
    CHECK_FALSE(agent_remove.ok);
    CHECK(agent_remove.reason == "human_edited");
}

// from tests/test_meeting_state.py::TestItemOps::test_pin_protects_item
TEST_CASE("TestItemOps::test_pin_protects_item", "[test_meeting_state]") {
    Store s;
    const auto added = s.one("agent", "agent",
                             R"([{"op": "add_item", "card": "risks", "text": "Timeline slip"}])");
    s.apply("user", "p_1", Json::array({{{"op", "pin_item"}, {"id", *added.target_id}}}));
    const auto blocked = s.apply("agent", "agent",
                                 Json::array({{{"op", "update_item"},
                                               {"id", *added.target_id},
                                               {"base_revision", 1},
                                               {"set", {{"text", "x"}}}}}))[0];
    CHECK_FALSE(blocked.ok);
    CHECK(blocked.reason == "human_edited");
}

// from tests/test_meeting_state.py::TestItemOps::test_remove_is_soft_delete
TEST_CASE("TestItemOps::test_remove_is_soft_delete", "[test_meeting_state]") {
    Store s;
    const auto added =
        s.one("user", "p_1", R"([{"op": "add_item", "card": "user_notes", "text": "temp"}])");
    const auto removed =
        s.apply("user", "p_1", Json::array({{{"op", "remove_item"}, {"id", *added.target_id}}}))[0];
    CHECK(removed.ok);
    CHECK(removed.effect["item"]["status"] == "removed");
    // Still findable for undo purposes.
    std::vector<std::string> texts;
    const auto snapshot = s.store->snapshot();
    for (const auto& item : snapshot["cards"]["user_notes"]) {
        texts.push_back(item["text"].get<std::string>());
    }
    CHECK(std::find(texts.begin(), texts.end(), "temp") != texts.end());
}

// from tests/test_meeting_state.py::TestItemOps::test_unknown_card_and_evidence_rejected
TEST_CASE("TestItemOps::test_unknown_card_and_evidence_rejected", "[test_meeting_state]") {
    Store s;
    const auto bad_card =
        s.one("agent", "agent", R"([{"op": "add_item", "card": "nope", "text": "x"}])");
    CHECK(bad_card.reason == "unknown_card");
    const auto bad_evidence = s.one("agent", "agent", R"([{"op": "add_item", "card": "key_points",
        "text": "x", "evidence": ["sg_missing"]}])");
    CHECK(bad_evidence.reason == "unknown_evidence");
}

// TestAgentBoundaries

// from tests/test_meeting_state.py::TestAgentBoundaries::test_agent_claim_requires_evidence
TEST_CASE("TestAgentBoundaries::test_agent_claim_requires_evidence", "[test_meeting_state]") {
    Store s;
    // Straight to the store: no fixture evidence is added.
    const auto result = s.store->apply(
        "agent", "agent",
        ops(R"([{"op": "add_item", "card": "decisions", "text": "Unsupported"}])"))[0];
    CHECK(result.ok == false);
    CHECK(result.reason == "missing_evidence");
}

// from
// tests/test_meeting_state.py::TestAgentBoundaries::test_persistence_failure_rejects_without_state_or_notification
TEST_CASE("TestAgentBoundaries::test_persistence_failure_rejects_without_state_or_notification",
          "[test_meeting_state]") {
    fv::FakeClock clock;
    auto repo = std::make_shared<testing::FakeMeetingRepository>(clock);
    repo->fail_on_ops_applied("database unavailable");
    SegmentPredicates predicates;
    predicates.segment_exists = [](std::string_view id) {
        return id == "sg_known";
    };
    testing::FixedIds ids;
    MeetingStateStore store(state_for("m_test"), repo, {}, clock, ids, predicates);
    std::vector<std::pair<std::int64_t, std::size_t>> notifications;
    const auto subscription =
        store.subscribe([&](std::int64_t seq, const std::vector<OpResult>& applied) {
            notifications.emplace_back(seq, applied.size());
        });

    const auto result = store.apply("agent", "agent", ops(R"([{"op": "add_item",
        "card": "decisions", "text": "Ship", "evidence": ["sg_known"]}])"))[0];

    CHECK(result.ok == false);
    CHECK(result.reason == "persistence_error");
    CHECK_FALSE(result.seq.has_value());
    CHECK(store.seq() == 0);
    CHECK(store.snapshot()["cards"]["decisions"] == Json::array());
    CHECK(notifications.empty());
}

// from tests/test_meeting_state.py::TestAgentBoundaries::test_agent_cannot_use_human_ops
TEST_CASE("TestAgentBoundaries::test_agent_cannot_use_human_ops", "[test_meeting_state]") {
    Store s;
    for (
        const char* op : {
            R"({"op": "pin_item", "id": "it_x"})",
            R"({"op": "rename_participant", "participant_id": "p_x", "display_name": "Sam"})",
            R"({"op": "answer_question", "question_id": "q_x", "answer_text": "yes"})",
            R"({"op": "set_cloud_enabled", "enabled": false})",
            R"({"op": "reassign_segment_speaker", "segment_id": "sg_known", "participant_id": null})",
        }) {
        const auto result = s.apply("agent", "agent", Json::array({Json::parse(op)}))[0];
        CHECK_FALSE(result.ok);
        CHECK(result.reason == "agent_forbidden");
    }
}

// from tests/test_meeting_state.py::TestAgentBoundaries::test_unknown_op_rejected
TEST_CASE("TestAgentBoundaries::test_unknown_op_rejected", "[test_meeting_state]") {
    Store s;
    CHECK(s.one("agent", "agent", R"([{"op": "replace_everything"}])").reason == "unknown_op");
}

// from tests/test_meeting_state.py::TestAgentBoundaries::test_rejected_ops_do_not_bump_seq
TEST_CASE("TestAgentBoundaries::test_rejected_ops_do_not_bump_seq", "[test_meeting_state]") {
    Store s;
    s.apply("agent", "agent", ops(R"([{"op": "unknown_thing"}])"));
    CHECK(s.store->seq() == 0);
}

// TestAgentAuthorityLimits

// from tests/test_meeting_state.py::TestAgentAuthorityLimits::test_agent_cannot_add_to_user_notes
TEST_CASE("TestAgentAuthorityLimits::test_agent_cannot_add_to_user_notes", "[test_meeting_state]") {
    Store s;
    const auto result = s.one(
        "agent", "agent", R"([{"op": "add_item", "card": "user_notes", "text": "Agent note"}])");
    CHECK_FALSE(result.ok);
    CHECK(result.reason == "human_only_card");
    CHECK(s.store->snapshot()["cards"]["user_notes"] == Json::array());
    // Humans still own the card.
    CHECK(s.one("user", "p_1", R"([{"op": "add_item", "card": "user_notes", "text": "My note"}])")
              .ok);
}

// from
// tests/test_meeting_state.py::TestAgentAuthorityLimits::test_agent_participants_stay_provisional
TEST_CASE("TestAgentAuthorityLimits::test_agent_participants_stay_provisional",
          "[test_meeting_state]") {
    Store s;
    const auto created = s.one("agent", "agent", R"([{"op": "upsert_participant",
        "display_name": "Sam", "is_provisional": false}])");
    CHECK(created.ok);
    CHECK(created.effect["participant"]["is_provisional"] == true);

    const auto renamed = s.apply("agent", "agent",
                                 Json::array({{{"op", "upsert_participant"},
                                               {"id", *created.target_id},
                                               {"display_name", "Samantha"},
                                               {"is_provisional", false}}}))[0];
    CHECK(renamed.ok);
    CHECK(renamed.effect["participant"]["is_provisional"] == true);
}

// from
// tests/test_meeting_state.py::TestAgentAuthorityLimits::test_agent_may_only_mint_others_clusters
TEST_CASE("TestAgentAuthorityLimits::test_agent_may_only_mint_others_clusters",
          "[test_meeting_state]") {
    Store s;
    for (const char* kind : {"me", "guest", "nonsense"}) {
        const auto result = s.apply(
            "agent", "agent",
            Json::array(
                {{{"op", "upsert_participant"}, {"display_name", "Impostor"}, {"kind", kind}}}))[0];
        INFO(kind);
        CHECK_FALSE(result.ok);
        CHECK(result.reason == "invalid_kind");
    }
    CHECK(s.store->snapshot()["participants"] == Json::object());

    const auto allowed = s.one("agent", "agent", R"([{"op": "upsert_participant",
        "display_name": "Speaker 2", "kind": "others_cluster"}])");
    CHECK(allowed.ok);
    CHECK(allowed.effect["participant"]["kind"] == "others_cluster");
}

// from
// tests/test_meeting_state.py::TestAgentAuthorityLimits::test_humans_create_non_provisional_participants
TEST_CASE("TestAgentAuthorityLimits::test_humans_create_non_provisional_participants",
          "[test_meeting_state]") {
    Store s;
    const auto result =
        s.one("host", "p_me", R"([{"op": "upsert_participant", "display_name": "Sarah"}])");
    CHECK(result.ok);
    CHECK(result.effect["participant"]["kind"] == "others_cluster");
    CHECK(result.effect["participant"]["is_provisional"] == false);
}

// from
// tests/test_meeting_state.py::TestAgentAuthorityLimits::test_only_the_app_may_mint_me_and_guest_identities
TEST_CASE("TestAgentAuthorityLimits::test_only_the_app_may_mint_me_and_guest_identities",
          "[test_meeting_state]") {
    // A dashboard client minting "me" could impersonate the host; the engine creates both
    // kinds as the system.
    Store s;
    for (const char* actor : {"host", "user"}) {
        for (const char* kind : {"me", "guest"}) {
            const auto blocked = s.apply(actor, "p_1",
                                         Json::array({{{"op", "upsert_participant"},
                                                       {"display_name", "Imposter"},
                                                       {"kind", kind}}}))[0];
            CHECK_FALSE(blocked.ok);
            CHECK(blocked.reason == "invalid_kind");
        }
    }
    const auto allowed =
        s.one("system", std::nullopt,
              R"([{"op": "upsert_participant", "display_name": "Me", "kind": "me"}])");
    CHECK(allowed.ok);
    CHECK(allowed.effect["participant"]["kind"] == "me");
}

// from
// tests/test_meeting_state.py::TestAgentAuthorityLimits::test_blank_rolling_summary_cannot_wipe_the_summary
TEST_CASE("TestAgentAuthorityLimits::test_blank_rolling_summary_cannot_wipe_the_summary",
          "[test_meeting_state]") {
    Store s;
    CHECK(s.one("agent", "agent",
                R"([{"op": "set_rolling_summary", "text": "We agreed to ship on Friday."}])")
              .ok);
    // The agent is rejected on content; a guest never reaches the handler, the summary being
    // host-only metadata.
    struct Case {
        const char* actor;
        const char* text;
        const char* reason;
    };
    for (const Case& c :
         {Case{"agent", "", "invalid_text"}, Case{"agent", "   \n ", "invalid_text"},
          Case{"user", "", "host_only"}, Case{"user", "a guest rewrite", "host_only"}}) {
        const auto wiped = s.apply(
            c.actor, "p_1", Json::array({{{"op", "set_rolling_summary"}, {"text", c.text}}}))[0];
        CHECK_FALSE(wiped.ok);
        CHECK(wiped.reason == c.reason);
    }
    CHECK(s.store->snapshot()["rolling_summary"] == "We agreed to ship on Friday.");
}

// TestEvidenceIntegrity

// from
// tests/test_meeting_state.py::TestEvidenceIntegrity::test_update_unions_evidence_instead_of_replacing_it
TEST_CASE("TestEvidenceIntegrity::test_update_unions_evidence_instead_of_replacing_it",
          "[test_meeting_state]") {
    Store s;
    s.with_segments({"sg_a", "sg_b", "sg_c"});
    const auto added = s.one("agent", "agent", R"([{"op": "add_item", "card": "key_points",
        "text": "Original", "evidence": ["sg_known", "sg_a"]}])");
    // A consolidation-style reword citing only its newest anchors.
    const auto updated = s.apply("agent", "agent",
                                 Json::array({{{"op", "update_item"},
                                               {"id", *added.target_id},
                                               {"base_revision", 1},
                                               {"set", {{"text", "Reworded and merged"}}},
                                               {"evidence", {"sg_b", "sg_a", "sg_c"}}}}))[0];
    CHECK(updated.ok);
    CHECK(updated.effect["item"]["evidence"] == Json::array({"sg_known", "sg_a", "sg_b", "sg_c"}));
}

// from tests/test_meeting_state.py::TestEvidenceIntegrity::test_undo_restores_the_previous_evidence
TEST_CASE("TestEvidenceIntegrity::test_undo_restores_the_previous_evidence",
          "[test_meeting_state]") {
    Store s;
    s.with_segments({"sg_a", "sg_b"});
    const auto added = s.one("agent", "agent", R"([{"op": "add_item", "card": "decisions",
        "text": "Original", "evidence": ["sg_known", "sg_a"]}])");
    const auto updated = s.apply("agent", "agent",
                                 Json::array({{{"op", "update_item"},
                                               {"id", *added.target_id},
                                               {"base_revision", 1},
                                               {"set", {{"text", "Reworded"}}},
                                               {"evidence", {"sg_b"}}}}))[0];
    const auto undo = s.store->undo(*updated.seq, "p_host");
    REQUIRE_FALSE(undo.empty());
    CHECK(undo[0].ok);
    const Json& item = undo[0].effect["item"];
    CHECK(item["text"] == "Original");
    CHECK(item["evidence"] == Json::array({"sg_known", "sg_a"}));
}

// TestParticipants

// from tests/test_meeting_state.py::TestParticipants::test_agent_creates_provisional_participant
TEST_CASE("TestParticipants::test_agent_creates_provisional_participant", "[test_meeting_state]") {
    Store s;
    const auto result =
        s.one("agent", "agent", R"([{"op": "upsert_participant", "display_name": "Sam"}])");
    CHECK(result.ok);
    CHECK(result.effect["participant"]["name_source"] == "agent_inferred");
    CHECK(result.effect["participant"]["is_provisional"] == true);
}

// from tests/test_meeting_state.py::TestParticipants::test_human_rename_blocks_agent_suggestions
TEST_CASE("TestParticipants::test_human_rename_blocks_agent_suggestions", "[test_meeting_state]") {
    Store s;
    const auto created =
        s.one("agent", "agent", R"([{"op": "upsert_participant", "display_name": "Speaker 1"}])");
    const std::string pid = *created.target_id;
    s.apply("user", "p_me",
            Json::array({{{"op", "rename_participant"},
                          {"participant_id", pid},
                          {"display_name", "Sarah Chen"}}}));
    const auto suggestion = s.apply("agent", "agent",
                                    Json::array({{{"op", "suggest_participant_name"},
                                                  {"participant_id", pid},
                                                  {"display_name", "Sara"}}}))[0];
    CHECK_FALSE(suggestion.ok);
    CHECK(suggestion.reason == "human_named");
    CHECK(s.store->snapshot()["participants"][pid]["display_name"] == "Sarah Chen");
}

// TestQuestions

// from tests/test_meeting_state.py::TestQuestions::test_open_question_cap_applies_to_agent
TEST_CASE("TestQuestions::test_open_question_cap_applies_to_agent", "[test_meeting_state]") {
    Store s;
    for (int i = 0; i < 7; ++i) {
        CHECK(s.apply("agent", "agent",
                      Json::array({{{"op", "ask_question"},
                                    {"text", "Question " + std::to_string(i) + "?"}}}))[0]
                  .ok);
    }
    const auto capped =
        s.one("agent", "agent", R"([{"op": "ask_question", "text": "One too many?"}])");
    CHECK_FALSE(capped.ok);
    CHECK(capped.reason == "question_limit");
}

// from tests/test_meeting_state.py::TestQuestions::test_resolution_confidence_thresholds
TEST_CASE("TestQuestions::test_resolution_confidence_thresholds", "[test_meeting_state]") {
    Store s;
    const auto ask = [&] {
        return *s.one("agent", "agent",
                      R"([{"op": "ask_question", "text": "Is the deadline fixed?"}])")
                    .target_id;
    };
    const auto resolve = [&](const std::string& qid, const char* answer, double confidence) {
        return s.apply("agent", "agent",
                       Json::array({{{"op", "resolve_question"},
                                     {"question_id", qid},
                                     {"answer_text", answer},
                                     {"confidence", confidence}}}))[0];
    };
    // High confidence: resolved with the audio badge.
    const auto high = resolve(ask(), "Yes, end of Q3", 0.9);
    CHECK(high.ok);
    CHECK(high.effect["question"]["status"] == "resolved");
    CHECK(high.effect["question"]["answer_source"] == "audio");
    // Medium confidence: a greyed suggestion, still open.
    const auto mid = resolve(ask(), "Probably Q3", 0.6);
    CHECK(mid.ok);
    CHECK(mid.effect["question"]["status"] == "open");
    CHECK(mid.effect["question"]["suggested_answer"] == "Probably Q3");
    // Low confidence: rejected outright.
    const auto low = resolve(ask(), "Maybe", 0.2);
    CHECK_FALSE(low.ok);
    CHECK(low.reason == "low_confidence");
}

// from tests/test_meeting_state.py::TestQuestions::test_human_answer_beats_agent_resolution
TEST_CASE("TestQuestions::test_human_answer_beats_agent_resolution", "[test_meeting_state]") {
    Store s;
    const std::string qid =
        *s.one("agent", "agent", R"([{"op": "ask_question", "text": "Who owns rollout?"}])")
             .target_id;
    const auto answered = s.apply(
        "user", "p_me",
        Json::array(
            {{{"op", "answer_question"}, {"question_id", qid}, {"answer_text", "I do"}}}))[0];
    CHECK(answered.ok);
    CHECK(answered.effect["question"]["answer_source"] == "user");
    const auto stale = s.apply("agent", "agent",
                               Json::array({{{"op", "resolve_question"},
                                             {"question_id", qid},
                                             {"answer_text", "Sam does"},
                                             {"confidence", 0.95}}}))[0];
    CHECK_FALSE(stale.ok);
    CHECK(stale.reason == "already_closed");
}

// TestSegmentOps

// from tests/test_meeting_state.py::TestSegmentOps::test_reassign_speaker_flows_through_handler
TEST_CASE("TestSegmentOps::test_reassign_speaker_flows_through_handler", "[test_meeting_state]") {
    Store s;
    const auto created =
        s.one("user", "p_me", R"([{"op": "upsert_participant", "display_name": "Sarah"}])");
    const std::string pid = *created.target_id;
    const auto result = s.apply("user", "p_me",
                                Json::array({{{"op", "reassign_segment_speaker"},
                                              {"segment_id", "sg_known"},
                                              {"participant_id", pid}}}))[0];
    CHECK(result.ok);
    CHECK(s.segment("sg_known")["speaker_participant_id"] == pid);
    CHECK(s.segment("sg_known")["speaker_pinned"] == true);
    // The handler supplied the inverse from the prior row.
    CHECK(result.inverse == Json({{"op", "reassign_segment_speaker"},
                                  {"segment_id", "sg_known"},
                                  {"participant_id", nullptr}}));
}

// TestStoreMechanics

// from tests/test_meeting_state.py::TestStoreMechanics::test_set_title_is_host_only
TEST_CASE("TestStoreMechanics::test_set_title_is_host_only", "[test_meeting_state]") {
    Store s;
    const auto guest = s.one("user", "p_guest", R"([{"op": "set_title", "text": "Guest rename"}])");
    CHECK(guest.ok == false);
    CHECK(guest.reason == "host_only");
    CHECK(s.store->snapshot()["title"] == "");
    const auto host = s.one("host", "p_me", R"([{"op": "set_title", "text": "Host title"}])");
    CHECK(host.ok == true);
    CHECK(s.store->snapshot()["title"] == "Host title");
}

// from tests/test_meeting_state.py::TestStoreMechanics::test_seq_increments_per_applied_op
TEST_CASE("TestStoreMechanics::test_seq_increments_per_applied_op", "[test_meeting_state]") {
    Store s;
    const auto results = s.apply("user", "p_1", ops(R"([
        {"op": "add_item", "card": "user_notes", "text": "a"},
        {"op": "add_item", "card": "user_notes", "text": "b"},
        {"op": "unknown_thing"}])"));
    REQUIRE(results.size() == 3);
    CHECK(results[0].seq == 1);
    CHECK(results[1].seq == 2);
    CHECK_FALSE(results[2].seq.has_value());
    CHECK(s.store->seq() == 2);
}

// from tests/test_meeting_state.py::TestStoreMechanics::test_subscribers_receive_applied_only
TEST_CASE("TestStoreMechanics::test_subscribers_receive_applied_only", "[test_meeting_state]") {
    Store s;
    std::vector<std::pair<std::int64_t, std::vector<OpResult>>> received;
    const auto subscription =
        s.store->subscribe([&](std::int64_t seq, const std::vector<OpResult>& results) {
            received.emplace_back(seq, results);
        });
    s.apply("user", "p_1", ops(R"([{"op": "add_item", "card": "user_notes", "text": "a"},
                                   {"op": "bad_op"}])"));
    REQUIRE(received.size() == 1);
    CHECK(received[0].first == 1);
    REQUIRE(received[0].second.size() == 1);
    CHECK(received[0].second[0].ok);
}

// from tests/test_meeting_state.py::TestStoreMechanics::test_undo_restores_previous_text
TEST_CASE("TestStoreMechanics::test_undo_restores_previous_text", "[test_meeting_state]") {
    Store s;
    const auto added =
        s.one("agent", "agent", R"([{"op": "add_item", "card": "decisions", "text": "Original"}])");
    const auto edited = s.apply("user", "p_1",
                                Json::array({{{"op", "update_item"},
                                              {"id", *added.target_id},
                                              {"set", {{"text", "Edited by human"}}}}}))[0];
    const auto undo = s.store->undo(*edited.seq, "p_host");
    REQUIRE_FALSE(undo.empty());
    CHECK(undo[0].ok);
    const Json item = item_in(s.store->snapshot(), "decisions", *added.target_id);
    CHECK(item["text"] == "Original");
    // Undo restored the pre-edit status too.
    CHECK(item["status"] == "proposed");
    // The same activity row cannot be applied twice; undoing the undo event redoes instead.
    CHECK(s.store->undo(*edited.seq, "p_host").empty());
}

// from tests/test_meeting_state.py::TestStoreMechanics::test_undo_of_add_removes_item
TEST_CASE("TestStoreMechanics::test_undo_of_add_removes_item", "[test_meeting_state]") {
    Store s;
    const auto added = s.one("agent", "agent",
                             R"([{"op": "add_item", "card": "key_points", "text": "Ephemeral"}])");
    const auto undo = s.store->undo(*added.seq, "p_host");
    REQUIRE_FALSE(undo.empty());
    CHECK(undo[0].ok);
    CHECK(item_in(s.store->snapshot(), "key_points", *added.target_id)["status"] == "removed");
}

// from tests/test_meeting_state.py::TestStoreMechanics::test_snapshot_round_trips
TEST_CASE("TestStoreMechanics::test_snapshot_round_trips", "[test_meeting_state]") {
    Store s;
    s.apply("agent", "agent", ops(R"([{"op": "set_topic", "text": "Q3 planning"},
                                      {"op": "add_item", "card": "key_points", "text": "Point"},
                                      {"op": "ask_question", "text": "Why?"}])"));
    const Json snapshot = s.store->snapshot();
    CHECK(MeetingState::from_json(snapshot).to_json() == snapshot);
    CHECK(snapshot["report_views"] == Json::array({"ribbon", "brief", "signal"}));
}

// from
// tests/test_meeting_state.py::TestStoreMechanics::test_report_views_survive_apply_and_legacy_default
TEST_CASE("TestStoreMechanics::test_report_views_survive_apply_and_legacy_default",
          "[test_meeting_state]") {
    MeetingState state = state_for("m_views");
    state.report_views = {"brief", "signal"};
    MeetingStateStore store(state);
    store.apply("agent", "agent",
                ops(R"([{"op": "set_topic", "text": "Q3 planning", "evidence": ["sg_known"]}])"));
    CHECK(store.snapshot()["report_views"] == Json::array({"brief", "signal"}));

    const MeetingState legacy = MeetingState::from_json({{"meeting_id", "m_legacy"}});
    CHECK(legacy.report_views == std::vector<std::string>{"ribbon", "brief", "signal"});
}

// TestClientAbusePrevention

// from
// tests/test_meeting_state.py::TestClientAbusePrevention::test_guest_cannot_forge_answered_from_audio
TEST_CASE("TestClientAbusePrevention::test_guest_cannot_forge_answered_from_audio",
          "[test_meeting_state]") {
    // resolve_question stamps answer_source "audio"; from a browser it would attribute a
    // guest's words to the recording. Humans answer with answer_question.
    Store s;
    const std::string qid =
        *s.one("agent", "agent", R"([{"op": "ask_question", "text": "Who owns rollout?"}])")
             .target_id;
    for (const char* actor : {"user", "host"}) {
        const auto forged = s.apply(actor, "p_guest",
                                    Json::array({{{"op", "resolve_question"},
                                                  {"question_id", qid},
                                                  {"answer_text", "fabricated"},
                                                  {"confidence", 1.0}}}))[0];
        CHECK_FALSE(forged.ok);
        CHECK(forged.reason == "agent_only");
    }
    const Json snapshot = s.store->snapshot();
    const auto question = std::find_if(snapshot["questions"].begin(), snapshot["questions"].end(),
                                       [&](const Json& q) { return q["id"] == qid; });
    REQUIRE(question != snapshot["questions"].end());
    CHECK((*question)["status"] == "open");
    CHECK((*question)["answer_source"].is_null());

    const auto answered = s.apply(
        "user", "p_guest",
        Json::array(
            {{{"op", "answer_question"}, {"question_id", qid}, {"answer_text", "I do"}}}))[0];
    CHECK(answered.ok);
    CHECK(answered.effect["question"]["answer_source"] == "user");
}

// from
// tests/test_meeting_state.py::TestClientAbusePrevention::test_guests_cannot_rewrite_meeting_metadata
TEST_CASE("TestClientAbusePrevention::test_guests_cannot_rewrite_meeting_metadata",
          "[test_meeting_state]") {
    Store s;
    for (const char* op : {R"({"op": "set_topic", "text": "hijacked"})",
                           R"({"op": "set_title", "text": "hijacked"})",
                           R"({"op": "set_rolling_summary", "text": "hijacked"})"}) {
        const auto blocked = s.apply("user", "p_guest", Json::array({Json::parse(op)}))[0];
        CHECK_FALSE(blocked.ok);
        CHECK(blocked.reason == "host_only");
        CHECK(s.apply("host", "p_me", Json::array({Json::parse(op)}))[0].ok);
    }
}

// from tests/test_meeting_state.py::TestClientAbusePrevention::test_oversized_item_data_is_rejected
TEST_CASE("TestClientAbusePrevention::test_oversized_item_data_is_rejected",
          "[test_meeting_state]") {
    Store s;
    const Json huge = {{"note", std::string(8000, 'x')}};
    const auto added = s.apply(
        "user", "p_1",
        Json::array(
            {{{"op", "add_item"}, {"card", "user_notes"}, {"text", "note"}, {"data", huge}}}))[0];
    CHECK_FALSE(added.ok);
    CHECK(added.reason == "data_too_large");

    const auto ok_item =
        s.one("user", "p_1", R"([{"op": "add_item", "card": "user_notes", "text": "note"}])");
    const auto updated = s.apply(
        "user", "p_1",
        Json::array(
            {{{"op", "update_item"}, {"id", *ok_item.target_id}, {"set", {{"data", huge}}}}}))[0];
    CHECK_FALSE(updated.ok);
    CHECK(updated.reason == "data_too_large");
}

// from
// tests/test_meeting_state.py::TestClientAbusePrevention::test_participant_and_question_floods_are_capped
TEST_CASE("TestClientAbusePrevention::test_participant_and_question_floods_are_capped",
          "[test_meeting_state]") {
    Store s;
    for (std::size_t i = 0; i < MAX_PARTICIPANTS; ++i) {
        REQUIRE(s.apply("user", "p_1",
                        Json::array({{{"op", "upsert_participant"},
                                      {"display_name", "P" + std::to_string(i)}}}))[0]
                    .ok);
    }
    const auto flooded =
        s.one("user", "p_1", R"([{"op": "upsert_participant", "display_name": "one too many"}])");
    CHECK_FALSE(flooded.ok);
    CHECK(flooded.reason == "participant_limit");

    for (std::size_t i = 0; i < MAX_OPEN_QUESTIONS_HUMAN; ++i) {
        REQUIRE(s.apply("user", "p_1",
                        Json::array(
                            {{{"op", "ask_question"}, {"text", "Q" + std::to_string(i) + "?"}}}))[0]
                    .ok);
    }
    const auto capped =
        s.one("user", "p_1", R"([{"op": "ask_question", "text": "one too many?"}])");
    CHECK_FALSE(capped.ok);
    CHECK(capped.reason == "question_limit");
}

// TestPinnedSpeakerProtection: a human speaker correction outranks any later automated relabel.

// from
// tests/test_meeting_state.py::TestPinnedSpeakerProtection::test_diarizer_cannot_revert_a_human_pin
TEST_CASE("TestPinnedSpeakerProtection::test_diarizer_cannot_revert_a_human_pin",
          "[test_meeting_state]") {
    Store s(true);
    const auto sarah =
        s.one("user", "p_me", R"([{"op": "upsert_participant", "display_name": "Sarah"}])");
    const auto dana =
        s.one("user", "p_me", R"([{"op": "upsert_participant", "display_name": "Dana"}])");
    const auto pinned = s.apply("user", "p_me",
                                Json::array({{{"op", "reassign_segment_speaker"},
                                              {"segment_id", "sg_known"},
                                              {"participant_id", *sarah.target_id}}}))[0];
    CHECK(pinned.ok);
    CHECK(s.segment("sg_known")["speaker_pinned"] == true);

    // A re-cluster batch computed before the pin landed.
    const auto stale = s.apply("system", "diarizer",
                               Json::array({{{"op", "reassign_segment_speaker"},
                                             {"segment_id", "sg_known"},
                                             {"participant_id", *dana.target_id}}}))[0];
    CHECK_FALSE(stale.ok);
    CHECK(stale.reason == "segment_pinned");
    CHECK(s.segment("sg_known")["speaker_participant_id"] == *sarah.target_id);
    CHECK(s.segment("sg_known")["speaker_pinned"] == true);
}

// from
// tests/test_meeting_state.py::TestPinnedSpeakerProtection::test_a_later_human_correction_still_wins
TEST_CASE("TestPinnedSpeakerProtection::test_a_later_human_correction_still_wins",
          "[test_meeting_state]") {
    Store s(true);
    const auto sarah =
        s.one("user", "p_me", R"([{"op": "upsert_participant", "display_name": "Sarah"}])");
    const auto dana =
        s.one("user", "p_me", R"([{"op": "upsert_participant", "display_name": "Dana"}])");
    s.apply("user", "p_me",
            Json::array({{{"op", "reassign_segment_speaker"},
                          {"segment_id", "sg_known"},
                          {"participant_id", *sarah.target_id}}}));
    const auto corrected = s.apply("host", "p_host",
                                   Json::array({{{"op", "reassign_segment_speaker"},
                                                 {"segment_id", "sg_known"},
                                                 {"participant_id", *dana.target_id}}}))[0];
    CHECK(corrected.ok);
    CHECK(s.segment("sg_known")["speaker_participant_id"] == *dana.target_id);
}

// TestFinalizationState: the persisted finalization contract.

// from tests/test_meeting_state.py::TestFinalizationState::test_round_trip_every_status
TEST_CASE("TestFinalizationState::test_round_trip_every_status", "[test_meeting_state]") {
    for (const std::string_view status : FINALIZATION_STATUSES) {
        MeetingState state = state_for("m_fin");
        state.cloud_enabled = true;
        state.finalization.status = std::string(status);
        state.finalization.message = "msg-" + std::string(status);
        const MeetingState rebuilt = MeetingState::from_json(state.to_json());
        CHECK(rebuilt.finalization.status == status);
        CHECK(rebuilt.finalization.message == "msg-" + std::string(status));
    }
}

// from tests/test_meeting_state.py::TestFinalizationState::test_legacy_payload_inference
TEST_CASE("TestFinalizationState::test_legacy_payload_inference", "[test_meeting_state]") {
    CHECK(MeetingState::from_json(
              Json::parse(R"({"meeting_id": "m1", "cloud_enabled": false, "status": "ended"})"))
              .finalization.status == "disabled");
    CHECK(MeetingState::from_json(
              Json::parse(R"({"meeting_id": "m2", "cloud_enabled": true, "status": "active"})"))
              .finalization.status == "pending");
    CHECK(MeetingState::from_json(
              Json::parse(R"({"meeting_id": "m3", "cloud_enabled": true, "status": "ended"})"))
              .finalization.status == "unavailable");
}

// from tests/test_meeting_state.py::TestFinalizationState::test_malformed_payload_falls_back
TEST_CASE("TestFinalizationState::test_malformed_payload_falls_back", "[test_meeting_state]") {
    CHECK(MeetingState::from_json(Json::parse(R"({"meeting_id": "m4", "cloud_enabled": true,
              "status": "active", "finalization": {"status": "nope", "message": "x"}})"))
              .finalization.status == "pending");
    CHECK(MeetingState::from_json(Json::parse(R"({"meeting_id": "m5", "cloud_enabled": false,
              "status": "ended", "finalization": "broken"})"))
              .finalization.status == "disabled");
}

// from
// tests/test_meeting_state.py::TestFinalizationState::test_normalize_historical_interrupted_terminal
TEST_CASE("TestFinalizationState::test_normalize_historical_interrupted_terminal",
          "[test_meeting_state]") {
    const auto running = FinalizationState::normalize_historical(
        Json::parse(R"({"status": "running", "message": "Preparing…"})"), true, "ended");
    CHECK(running.status == "failed");
    CHECK(fv::py::lower(running.message).find("interrupted") != std::string::npos);

    const auto pending = FinalizationState::normalize_historical(
        Json::parse(R"({"status": "pending", "message": ""})"), true, "needs_recovery");
    CHECK(pending.status == "unavailable");
    CHECK(fv::py::lower(pending.message).find("not recorded") != std::string::npos);

    const auto cloud_off = FinalizationState::normalize_historical(
        Json::parse(R"({"status": "running", "message": "…"})"), false, "ended");
    CHECK(cloud_off.status == "disabled");
}

// from
// tests/test_meeting_state.py::TestFinalizationState::test_normalize_historical_keeps_step_list
TEST_CASE("TestFinalizationState::test_normalize_historical_keeps_step_list",
          "[test_meeting_state]") {
    const auto interrupted = FinalizationState::normalize_historical(Json::parse(R"({
        "status": "running", "message": "Cleaning transcript…",
        "steps": [
            {"id": "redecode", "name": "Audio Re-transcription", "status": "completed", "detail": "Done"},
            {"id": "polish", "name": "Transcript Cleanup", "status": "running", "detail": "Cleaning…"}],
        "current_step": 2, "total_steps": 2})"),
                                                                     true, "ended");
    CHECK(interrupted.status == "failed");
    REQUIRE(interrupted.steps.size() == 2);
    CHECK(interrupted.steps[0]["status"] == "completed");
    CHECK(interrupted.steps[1]["status"] == "failed");
    CHECK(interrupted.current_step == 2);
}

// from
// tests/test_meeting_state.py::TestFinalizationState::test_normalize_historical_preserves_live_and_terminal
TEST_CASE("TestFinalizationState::test_normalize_historical_preserves_live_and_terminal",
          "[test_meeting_state]") {
    const auto live_running = FinalizationState::normalize_historical(
        Json::parse(R"({"status": "running", "message": "Preparing…"})"), true, "ending");
    CHECK(live_running.status == "running");
    CHECK(live_running.message == "Preparing…");

    const auto completed = FinalizationState::normalize_historical(
        Json::parse(R"({"status": "completed", "message": "Done."})"), true, "ended");
    CHECK(completed.status == "completed");
    CHECK(completed.message == "Done.");
}

// from
// tests/test_meeting_state.py::TestFinalizationState::test_card_deferred_round_trip_and_defaults
TEST_CASE("TestFinalizationState::test_card_deferred_round_trip_and_defaults",
          "[test_meeting_state]") {
    MeetingState state = state_for("m_defer");
    state.status = "ended";
    state.cloud_enabled = true;
    state.finalization.status = "failed";
    state.finalization.message = "boom";
    state.finalization.card_deferred = true;
    const MeetingState rebuilt = MeetingState::from_json(state.to_json());
    CHECK(rebuilt.finalization.card_deferred == true);
    CHECK(rebuilt.finalization.status == "failed");

    CHECK(FinalizationState::coerce(Json::parse(R"({"status": "failed", "message": "boom"})"), true,
                                    "ended")
              .card_deferred == false);
    CHECK(FinalizationState::coerce(
              Json::parse(R"({"status": "failed", "message": "boom", "card_deferred": "nope"})"),
              true, "ended")
              .card_deferred == false);
}

// from
// tests/test_meeting_state.py::TestFinalizationState::test_normalize_historical_preserves_card_deferred
TEST_CASE("TestFinalizationState::test_normalize_historical_preserves_card_deferred",
          "[test_meeting_state]") {
    const auto interrupted = FinalizationState::normalize_historical(
        Json::parse(R"({"status": "running", "message": "Preparing…", "card_deferred": true})"),
        true, "ended");
    CHECK(interrupted.status == "failed");
    CHECK(interrupted.card_deferred == true);

    const auto already_failed = FinalizationState::normalize_historical(
        Json::parse(R"({"status": "failed", "message": "boom", "card_deferred": true})"), true,
        "ended");
    CHECK(already_failed.status == "failed");
    CHECK(already_failed.card_deferred == true);
}

// from tests/test_meeting_state.py::TestFinalizationState::test_history_pill_precedence
TEST_CASE("TestFinalizationState::test_history_pill_precedence", "[test_meeting_state]") {
    using Pill = std::optional<std::pair<std::string, std::string>>;
    // FinalizationState(**fields): the fields as given, no coercion.
    const auto pill = [](const char* json, const char* status) {
        const Json fields = Json::parse(json);
        FinalizationState state;
        state.status = fields.at("status").get<std::string>();
        state.message = fields.value("message", "");
        state.steps = fields.value("steps", Json::array());
        state.card_deferred = fields.value("card_deferred", false);
        return state.history_pill(status);
    };
    CHECK(pill(R"({"status": "failed", "message": "boom", "card_deferred": true})", "ended") ==
          Pill({"Saved for later", "warning"}));
    CHECK(pill(R"({"status": "completed", "steps": [{"id": "polish", "status": "failed"}]})",
               "ended") == Pill({"Incomplete", "warning"}));
    CHECK(pill(R"({"status": "unavailable"})", "ended") == Pill({"Unavailable", "warning"}));
    CHECK(pill(R"({"status": "completed"})", "ended") == Pill({"Ready", "success"}));
    CHECK(pill(R"({"status": "completed", "card_deferred": true})", "ended") ==
          Pill({"Ready", "success"}));
    CHECK(pill(R"({"status": "disabled"})", "ended") == Pill({"Off", "neutral"}));
    CHECK(pill(R"({"status": "disabled", "card_deferred": true})", "ended") ==
          Pill({"Off", "neutral"}));
    CHECK(pill(R"({"status": "failed", "card_deferred": true})", "active") == std::nullopt);
}

// from tests/test_meeting_state.py::TestFinalizationState::test_step_progression_round_trip
TEST_CASE("TestFinalizationState::test_step_progression_round_trip", "[test_meeting_state]") {
    MeetingState state = state_for("m_steps");
    state.cloud_enabled = true;
    FinalizationState& fin = state.finalization;
    fin.status = "running";
    fin.message = "Cleaning transcript…";
    fin.stage = "polish";
    fin.current_step = 2;
    fin.total_steps = 3;
    fin.step_details = "Polishing block 1 of 2";
    fin.steps = Json::parse(R"([
        {"id": "redecode", "name": "Audio Re-transcription", "status": "completed", "detail": "Decoded 12 windows"},
        {"id": "polish", "name": "Transcript Cleanup", "status": "running", "detail": "Polishing block 1/2"},
        {"id": "consolidation", "name": "Summary & Action Items", "status": "pending", "detail": "Waiting"}])");
    fin.summary_stats = Json::parse(R"({"duration_s": 142.5, "segments": 35, "words": 420,
        "key_points": 4, "action_items": 2, "decisions": 1})");
    const MeetingState rebuilt = MeetingState::from_json(state.to_json());
    CHECK(rebuilt.finalization.stage == "polish");
    CHECK(rebuilt.finalization.current_step == 2);
    CHECK(rebuilt.finalization.total_steps == 3);
    CHECK(rebuilt.finalization.step_details == "Polishing block 1 of 2");
    REQUIRE(rebuilt.finalization.steps.size() == 3);
    CHECK(rebuilt.finalization.steps[0]["status"] == "completed");
    CHECK(rebuilt.finalization.summary_stats["segments"] == 35);
    CHECK(rebuilt.finalization.summary_stats["key_points"] == 4);
}

// Spoken notes

// from
// tests/test_meeting_state.py::test_spoken_note_survives_agent_updates_but_remains_editable_and_removable
TEST_CASE("test_spoken_note_survives_agent_updates_but_remains_editable_and_removable",
          "[test_meeting_state]") {
    Store s;
    const auto added = s.one("system", "voice_command", R"([{"op": "add_item", "card": "key_points",
        "text": "Budget A needs $1,000.", "evidence": ["sg_known"],
        "data": {"source": "voice_command", "command": "note_this"}}])");
    CHECK(added.ok);
    CHECK(added.effect["item"]["status"] == "proposed");
    for (const Json& op :
         {Json{{"op", "update_item"},
               {"id", *added.target_id},
               {"base_revision", 1},
               {"set", {{"text", "The participant tested voice commands."}}}},
          Json{{"op", "remove_item"}, {"id", *added.target_id}, {"base_revision", 1}}}) {
        const auto result = s.apply("agent", "agent", Json::array({op}))[0];
        CHECK_FALSE(result.ok);
        CHECK(result.reason == "human_edited");
    }
    CHECK(s.apply("host", "me",
                  Json::array({{{"op", "update_item"},
                                {"id", *added.target_id},
                                {"set", {{"text", "Budget A needs $2,000."}}}}}))[0]
              .ok);
    CHECK(s.apply("host", "me", Json::array({{{"op", "remove_item"}, {"id", *added.target_id}}}))[0]
              .ok);
}

// from
// tests/test_meeting_state.py::test_spoken_note_protection_survives_persistence_without_protecting_other_system_items
TEST_CASE("test_spoken_note_protection_survives_persistence_without_protecting_other_system_items",
          "[test_meeting_state]") {
    CardItem note;
    note.id = "spoken";
    note.card = "key_points";
    note.text = "Budget A needs $1,000.";
    note.author_type = "system";
    note.author_id = "voice_command";
    CHECK(CardItem::from_json(note.to_json()).is_protected());
    note.author_id = "state_repair";
    CHECK_FALSE(note.is_protected());
    note.author_type = "agent";
    note.author_id = "voice_command";
    CHECK_FALSE(note.is_protected());
}

// from tests/test_meeting_intent.py::TestIntentOp::test_undo_restores_the_previous_brief
TEST_CASE("TestIntentOp::test_undo_restores_the_previous_brief", "[test_meeting_intent]") {
    // A store writing through to a real repository, whose audit log undo reads.
    SqlFixture sql;
    make_meeting(*sql.repo, "m_intent");
    MeetingStateStore store(state_for("m_intent"), sql.repo, {}, sql.clock, sql.ids);
    const std::string brief = "We decide the vendor today. Capture who objected and why, and the "
                              "number attached to each bid.";
    store.apply("host", "p_host", Json::array({{{"op", "set_meeting_intent"}, {"text", brief}}}));
    const auto replaced = store.apply(
        "host", "p_host",
        Json::array({{{"op", "set_meeting_intent"}, {"text", "Never mind, just minutes"}}}))[0];

    const auto undone = store.undo(*replaced.seq, "p_host");

    REQUIRE_FALSE(undone.empty());
    CHECK(undone[0].ok);
    CHECK(store.snapshot()["intent"]["text"] == brief);
}

// test_meeting_cloud_speakers.py: the segment handler on a headless store.

namespace {

// _seed_meeting: an ended meeting with a loopback segment and a human-pinned one.
std::string seed_speaker_meeting(SqlMeetingRepository& repo) {
    repo.create_meeting({{"id", "m_spk"},
                         {"title", "Sync"},
                         {"status", "ended"},
                         {"started_at", "2026-01-01T09:00:00"},
                         {"host_token", "h"},
                         {"guest_token", "g"},
                         {"cloud_enabled", false},
                         {"spool_dir", "/tmp/spool"},
                         {"state_json", nullptr},
                         {"state_seq", 0}});
    TranscriptSegment lb = make_segment("m_spk", "sg_lb", 0.0, 2.0, "hello", "loopback");
    TranscriptSegment pin = make_segment("m_spk", "sg_pin", 2.0, 4.0, "pinned", "loopback");
    pin.speaker_source = "human";
    pin.speaker_pinned = true;
    repo.add_segments({lb, pin});
    return "m_spk";
}

SegmentPredicates repository_predicates(const std::shared_ptr<SqlMeetingRepository>& repo,
                                        const std::string& meeting_id) {
    SegmentPredicates predicates;
    predicates.segment_exists = [repo, meeting_id](std::string_view id) {
        return repo->segment_exists(meeting_id, id);
    };
    predicates.segment_pinned = [repo, meeting_id](std::string_view id) {
        const auto row = repo->get_segment(meeting_id, id);
        return row && row->at("speaker_pinned").get<bool>();
    };
    return predicates;
}

const char* kSpeakerOne = R"([{"op": "upsert_participant", "display_name": "Speaker 1",
    "kind": "others_cluster", "is_provisional": true}])";

} // namespace

// from
// tests/test_meeting_cloud_speakers.py::TestSegmentHandlerFix::test_reassign_succeeds_on_headless_store
TEST_CASE("TestSegmentHandlerFix::test_reassign_succeeds_on_headless_store",
          "[test_meeting_cloud_speakers]") {
    SqlFixture sql;
    const std::string meeting_id = seed_speaker_meeting(*sql.repo);
    MeetingStateStore store(state_for(meeting_id), sql.repo,
                            make_segment_handler(sql.repo, meeting_id), sql.clock, sql.ids,
                            repository_predicates(sql.repo, meeting_id));
    const auto created = store.apply("system", "diarizer", ops(kSpeakerOne));
    CHECK(created[0].ok);
    const std::string pid = created[0].effect["participant"]["id"].get<std::string>();
    const auto results = store.apply("system", "diarizer",
                                     Json::array({{{"op", "reassign_segment_speaker"},
                                                   {"segment_id", "sg_lb"},
                                                   {"participant_id", pid}}}));
    CHECK(results[0].ok);
    CHECK_FALSE(results[0].reason.has_value());
    const Json row = *sql.repo->get_segment(meeting_id, "sg_lb");
    CHECK(row["speaker_participant_id"] == pid);
    CHECK(row["speaker_source"] == "diarizer");
}

// from tests/test_meeting_cloud_speakers.py::TestSegmentHandlerFix::test_pinned_segment_is_rejected
TEST_CASE("TestSegmentHandlerFix::test_pinned_segment_is_rejected",
          "[test_meeting_cloud_speakers]") {
    SqlFixture sql;
    const std::string meeting_id = seed_speaker_meeting(*sql.repo);
    MeetingStateStore store(state_for(meeting_id), sql.repo,
                            make_segment_handler(sql.repo, meeting_id), sql.clock, sql.ids,
                            repository_predicates(sql.repo, meeting_id));
    const auto created = store.apply("system", "diarizer", ops(kSpeakerOne));
    const std::string pid = created[0].effect["participant"]["id"].get<std::string>();
    const auto results = store.apply("system", "diarizer",
                                     Json::array({{{"op", "reassign_segment_speaker"},
                                                   {"segment_id", "sg_pin"},
                                                   {"participant_id", pid}}}));
    CHECK(results[0].ok == false);
    CHECK(results[0].reason == "segment_pinned");
}

// from
// tests/test_meeting_cloud_speakers.py::TestSegmentHandlerFix::test_bulk_lookup_answers_a_whole_batch_in_one_query
TEST_CASE("TestSegmentHandlerFix::test_bulk_lookup_answers_a_whole_batch_in_one_query",
          "[test_meeting_cloud_speakers]") {
    SqlFixture sql;
    const std::string meeting_id = seed_speaker_meeting(*sql.repo);
    const SegmentLookup bulk = repository_segment_lookup(sql.repo, meeting_id);
    std::vector<std::set<std::string>> lookups;
    SegmentPredicates predicates;
    predicates.segment_lookup = [&](const std::vector<std::string>& ids) {
        lookups.emplace_back(ids.begin(), ids.end());
        return bulk(ids);
    };
    const auto per_id = [](std::string_view) -> bool {
        throw std::logic_error("cited ids must come from the bulk lookup");
    };
    predicates.segment_exists = per_id;
    predicates.segment_pinned = per_id;
    MeetingStateStore store(state_for(meeting_id), sql.repo,
                            make_segment_handler(sql.repo, meeting_id), sql.clock, sql.ids,
                            predicates);
    const auto created = store.apply("system", "diarizer", ops(kSpeakerOne));
    const std::string pid = created[0].effect["participant"]["id"].get<std::string>();
    Json batch = Json::array();
    for (const char* id : {"sg_lb", "sg_pin", "sg_gone"}) {
        batch.push_back(
            {{"op", "reassign_segment_speaker"}, {"segment_id", id}, {"participant_id", pid}});
    }
    const auto results = store.apply("system", "diarizer", batch);

    REQUIRE(lookups.size() == 1);
    CHECK(lookups[0] == std::set<std::string>{"sg_lb", "sg_pin", "sg_gone"});
    CHECK(results[0].ok == true);
    CHECK(results[1].reason == "segment_pinned");
    CHECK(results[2].ok == false);
}
