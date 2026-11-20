// revise_segment_text op validation and store application
// (tests/test_meeting_revise_segment_text.py).

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "meeting_state/patches.h"
#include "meeting_state/store.h"
#include "sql_support.h"

using fv::Json;
using namespace fv::meeting_state;
using namespace fv::meeting_state::test_support;

namespace {

std::string seed_meeting(SqlMeetingRepository& repo) {
    const std::string mid = "m_polish";
    repo.create_meeting({{"id", mid},
                         {"title", "t"},
                         {"status", "active"},
                         {"started_at", "2026-01-01T09:00:00"},
                         {"host_token", "h"},
                         {"guest_token", "g"},
                         {"cloud_enabled", true},
                         {"spool_dir", "spool"}});
    repo.add_segments({make_segment(mid, "sg_one", 1.0, 2.0, "helo world")});
    return mid;
}

} // namespace

// from tests/test_meeting_revise_segment_text.py::test_revise_segment_text_rejects_missing_evidence
TEST_CASE("test_revise_segment_text_rejects_missing_evidence",
          "[test_meeting_revise_segment_text]") {
    MeetingState state = state_for("m1");
    state.title = "t";
    OpContext ctx;
    ctx.actor_type = "agent";
    ctx.actor_id = "agent";
    ctx.segment_exists = [](std::string_view id) {
        return id == "sg_one";
    };
    const auto results = apply_ops(state, ctx, Json::parse(R"([{"op": "revise_segment_text",
        "segment_id": "sg_one", "text": "hello world", "evidence": ["sg_other"]}])"));
    CHECK_FALSE(results[0].ok);
    CHECK((results[0].reason == "missing_evidence" || results[0].reason == "unknown_evidence"));
}

// from
// tests/test_meeting_revise_segment_text.py::test_revise_segment_text_rejects_untrusted_human_client
TEST_CASE("test_revise_segment_text_rejects_untrusted_human_client",
          "[test_meeting_revise_segment_text]") {
    MeetingState state = state_for("m1");
    state.title = "t";
    OpContext ctx;
    ctx.actor_type = "user";
    ctx.actor_id = "guest";
    ctx.segment_exists = [](std::string_view id) {
        return id == "sg_one";
    };
    const auto results = apply_ops(state, ctx, Json::parse(R"([{"op": "revise_segment_text",
        "segment_id": "sg_one", "text": "forged replacement"}])"));
    CHECK_FALSE(results[0].ok);
    CHECK(results[0].reason == "agent_only");
}

// from tests/test_meeting_revise_segment_text.py::test_revise_segment_text_applies_via_store
TEST_CASE("test_revise_segment_text_applies_via_store", "[test_meeting_revise_segment_text]") {
    SqlFixture f;
    const std::string mid = seed_meeting(*f.repo);
    MeetingState state = state_for(mid);
    state.title = "t";
    auto repo = f.repo;
    // Enriches the effect as the engine does, then returns the fixed inverse.
    SegmentHandler handler = [repo, mid](OpResult& result) -> Json {
        const Json effect = result.effect;
        Json updated = *repo->get_segment(mid, effect.at("segment_id").get<std::string>());
        updated["text"] = effect.at("text");
        result.effect = {{"entity", "segment_text"},
                         {"segment_id", effect.at("segment_id")},
                         {"text", effect.at("text")},
                         {"segment", updated}};
        return {{"op", "revise_segment_text"},
                {"segment_id", "sg_one"},
                {"text", "helo world"},
                {"evidence", {"sg_one"}}};
    };
    SegmentPredicates predicates;
    predicates.segment_exists = [repo, mid](std::string_view id) {
        return repo->segment_exists(mid, id);
    };
    MeetingStateStore store(state, f.repo, handler, f.clock, f.ids, predicates);
    const auto results = store.apply("agent", "agent", Json::parse(R"([{"op": "revise_segment_text",
        "segment_id": "sg_one", "text": "hello world", "evidence": ["sg_one"]}])"));
    CHECK(results[0].ok);
    CHECK(results[0].effect["entity"] == "segment_text");
    CHECK(f.repo->get_segment(mid, "sg_one")->at("text") == "hello world");
    // The repository aligned the broadcast payload with the stored row.
    CHECK(results[0].effect["segment"]["original_text"] == "hello world");
}

TEST_CASE("the shared segment handler undoes a revision to the stored raw text",
          "[meeting_state_store]") {
    SqlFixture f;
    const std::string mid = seed_meeting(*f.repo);
    // A meeting-wide correction makes reads differ from the stored words.
    f.repo->persist_state(mid, Json::parse(R"({"cards": {"user_notes": [{"id": "it_c",
        "card": "user_notes", "status": "edited", "author_type": "user", "text": "fix",
        "data": {"kind": "term_correction", "selected_text": "helo", "replacement": "hullo"}}]}})"));
    CHECK(f.repo->get_segment(mid, "sg_one")->at("text") == "hullo world");
    MeetingStateStore store(state_for(mid), f.repo, make_segment_handler(f.repo, mid), f.clock,
                            f.ids);
    const auto revised = store.apply("agent", "agent", Json::parse(R"([{"op": "revise_segment_text",
        "segment_id": "sg_one", "text": "hello there", "evidence": ["sg_one"]}])"))[0];
    REQUIRE(revised.ok);
    CHECK(revised.inverse == Json::parse(R"({"op": "revise_segment_text", "segment_id": "sg_one",
        "text": "helo world", "evidence": ["sg_one"]})"));
    CHECK(revised.effect["segment"]["original_text"] == "hello there");
    const auto undone = store.undo(*revised.seq, std::nullopt);
    REQUIRE(undone.size() == 1);
    CHECK(undone[0].ok);
    CHECK(f.repo->get_segment(mid, "sg_one")->at("original_text") == "helo world");

    const auto reassigned = store.apply("system", "diarizer", Json::parse(R"([{"op":
        "reassign_segment_speaker", "segment_id": "sg_one", "participant_id": null}])"))[0];
    REQUIRE(reassigned.ok);
    CHECK(reassigned.inverse == Json::parse(R"({"op": "reassign_segment_speaker",
        "segment_id": "sg_one", "participant_id": null, "_source": "channel", "_pinned": false,
        "force": true})"));
}
