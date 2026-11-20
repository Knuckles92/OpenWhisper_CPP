// tests/test_meeting_scoped_corrections.py: a human passage correction is anchored, reversible
// and exported consistently. Two assertions of the second test read corrections.guidance_prompt,
// the agent's prompt block, which is M8's; they are left out here.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "meeting_state/corrections.h"
#include "meeting_state/export_bulk.h"
#include "meeting_state/schema.h"
#include "meeting_state/segment_ops.h"
#include "meeting_state/store.h"
#include "pyport/json.h"
#include "sql_support.h"
#include "stub_repository.h"

using fv::Json;
using namespace fv::meeting_state;
using namespace fv::meeting_state::test_support;

namespace {

Json scoped_op(const std::string& segment_id, int index, const std::string& replacement,
               const std::string& source = "word", const std::string& base = "word word word") {
    return {{"op", "add_item"},
            {"card", "user_notes"},
            {"text", "Correct occurrence " + std::to_string(index)},
            {"evidence", {segment_id}},
            {"data",
             {{"kind", "occurrence_correction"},
              {"selected_text", source},
              {"replacement", replacement},
              {"occurrence_index", index},
              {"base_fingerprint", segment_fingerprint(base)}}}};
}

struct Fixture : SqlFixture {
    std::string meeting_id = make_meeting(*repo);
    std::unique_ptr<MeetingStateStore> store;

    void open(bool with_segment_handler = false) {
        SegmentPredicates predicates;
        predicates.segment_exists = [this](std::string_view id) {
            return repo->segment_exists(meeting_id, id);
        };
        store = std::make_unique<MeetingStateStore>(
            state_for(meeting_id), repo,
            with_segment_handler ? make_segment_handler(repo, meeting_id) : SegmentHandler{}, clock,
            ids, predicates);
    }

    OpResult host(const Json& op) {
        return store->apply("host", std::string("host"), Json::array({op}))[0];
    }

    std::vector<std::string> texts() {
        std::vector<std::string> out;
        for (const auto& row : repo->get_segments(meeting_id)) {
            out.push_back(row["text"].get<std::string>());
        }
        return out;
    }

    Json segment(const std::string& id) { return *repo->get_segment(meeting_id, id); }

    void set_raw_text(const std::string& id, const std::string& text) {
        database->write([&](fv::sql::Connection& connection) {
            auto statement =
                connection.prepare("UPDATE meeting_segments SET text = ? WHERE id = ?");
            statement.bind(1, text);
            statement.bind(2, id);
            statement.step();
        });
    }

    std::string exported(std::string_view format) {
        const auto entry = collect_meeting_export(*repo, meeting_id);
        REQUIRE(entry);
        return render_combined({*entry}, format, {}, clock);
    }
};

Json global_note(const std::string& text, const std::string& selected,
                 const std::string& replacement) {
    return {
        {"op", "add_item"},
        {"card", "user_notes"},
        {"text", text},
        {"data",
         {{"kind", "term_correction"}, {"selected_text", selected}, {"replacement", replacement}}}};
}

bool has(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

// from tests/test_meeting_scoped_corrections.py::test_fingerprint_is_stable_for_utf8_text
TEST_CASE("test_fingerprint_is_stable_for_utf8_text", "[test_meeting_scoped_corrections]") {
    CHECK(segment_fingerprint("alpha \xCE\xB2"
                              "eta \xF0\x9F\x98\x80") == "fnv1a64:8ea367c29aed5110");
}

// from
// tests/test_meeting_scoped_corrections.py::test_scoped_corrections_are_stable_persisted_exported_and_individually_undoable
TEST_CASE("test_scoped_corrections_are_stable_persisted_exported_and_individually_undoable",
          "[test_meeting_scoped_corrections]") {
    Fixture f;
    f.repo->add_segments({make_segment(f.meeting_id, "sg_one", 1, 3, "word word word"),
                          make_segment(f.meeting_id, "sg_two", 4, 5, "word")});
    f.open();
    const auto third = f.host(scoped_op("sg_one", 2, "third"));
    const auto first = f.host(scoped_op("sg_one", 0, "first"));
    REQUIRE(third.ok);
    REQUIRE(first.ok);
    CHECK(f.texts() == std::vector<std::string>{"first word third", "word"});
    CHECK(f.segment("sg_one")["original_text"] == "word word word");
    CHECK(term_rules(f.store->snapshot()).empty());
    // A fresh state store reads the same persisted note state after recovery.
    const Json recovered =
        MeetingState::from_json(
            fv::py::loads((*f.repo->get_meeting(f.meeting_id))["state_json"].get<std::string>()),
            f.clock)
            .to_json();
    CHECK(correct_segment_text("word word word", "sg_one", term_rules(recovered),
                               occurrence_rules(recovered)) == "first word third");
    CHECK(has(f.exported(FORMAT_TXT), "first word third"));
    CHECK(has(f.exported(FORMAT_MARKDOWN), "first word third"));
    const Json payload = fv::py::loads(f.exported(FORMAT_JSON))["meetings"][0];
    CHECK(payload["segments"][0]["text"] == "first word third");
    CHECK(payload["segments"][0]["original_text"] == "word word word");

    CHECK(f.store->undo(*first.seq, "host")[0].ok);
    CHECK(f.texts() == std::vector<std::string>{"word word third", "word"});
    CHECK(f.store->undo(*third.seq, "host")[0].ok);
    CHECK(f.texts() == std::vector<std::string>{"word word word", "word"});
    CHECK(has(f.exported(FORMAT_TXT), "word word word"));
}

// from
// tests/test_meeting_scoped_corrections.py::test_invalid_anchor_rejected_and_explicit_meeting_wide_rule_remains_global
TEST_CASE("test_invalid_anchor_rejected_and_explicit_meeting_wide_rule_remains_global",
          "[test_meeting_scoped_corrections]") {
    Fixture f;
    f.repo->add_segments({make_segment(f.meeting_id, "sg_one", 1, 3, "word word"),
                          make_segment(f.meeting_id, "sg_two", 4, 5, "word")});
    f.open();
    CHECK_FALSE(f.host(scoped_op("missing", 0, "once")).ok);
    Json unanchored = scoped_op("sg_one", 0, "once");
    unanchored["evidence"] = Json::array();
    CHECK_FALSE(f.host(unanchored).ok);
    CHECK(f.texts() == std::vector<std::string>{"word word", "word"});
    const auto added = f.host(global_note("Correct throughout meeting", "word", "global"));
    REQUIRE(added.ok);
    CHECK(f.texts() == std::vector<std::string>{"global global", "global"});
    CHECK(f.store->undo(*added.seq, "host")[0].ok);
    CHECK(f.texts() == std::vector<std::string>{"word word", "word"});
}

// from
// tests/test_meeting_scoped_corrections.py::test_new_global_rule_or_segment_revision_disables_stale_occurrence_until_base_returns
TEST_CASE("test_new_global_rule_or_segment_revision_disables_stale_occurrence_until_base_returns",
          "[test_meeting_scoped_corrections]") {
    Fixture f;
    const std::string raw = "alpha beta alpha";
    f.repo->add_segments({make_segment(f.meeting_id, "sg_one", 1, 3, raw)});
    f.open();
    REQUIRE(f.host(scoped_op("sg_one", 1, "fixed", "alpha", raw)).ok);
    CHECK(f.segment("sg_one")["text"] == "alpha beta fixed");
    const auto added = f.host(global_note("Correct beta everywhere", "beta", "alpha"));
    REQUIRE(added.ok);
    CHECK(f.segment("sg_one")["text"] == "alpha alpha alpha");
    CHECK(f.store->undo(*added.seq, "host")[0].ok);
    CHECK(f.segment("sg_one")["text"] == "alpha beta fixed");

    f.set_raw_text("sg_one", "alpha new beta alpha");
    CHECK(f.segment("sg_one")["text"] == "alpha new beta alpha");
    f.set_raw_text("sg_one", raw);
    CHECK(f.segment("sg_one")["text"] == "alpha beta fixed");

    const Json recovered =
        MeetingState::from_json(
            fv::py::loads((*f.repo->get_meeting(f.meeting_id))["state_json"].get<std::string>()),
            f.clock)
            .to_json();
    CHECK(correct_segment_text(raw, "sg_one", term_rules(recovered), occurrence_rules(recovered)) ==
          "alpha beta fixed");
}

// from
// tests/test_meeting_scoped_corrections.py::test_missing_or_invalid_base_fingerprint_is_rejected
TEST_CASE("test_missing_or_invalid_base_fingerprint_is_rejected",
          "[test_meeting_scoped_corrections]") {
    Fixture f;
    f.repo->add_segments({make_segment(f.meeting_id, "sg_one", 1, 3, "word word word")});
    f.open();
    Json op = scoped_op("sg_one", 1, "fixed");
    op["data"].erase("base_fingerprint");
    CHECK_FALSE(f.host(op).ok);
    op["data"]["base_fingerprint"] = "fnv1a64:wrong";
    CHECK_FALSE(f.host(op).ok);
    CHECK(f.segment("sg_one")["text"] == "word word word");
}

// from
// tests/test_meeting_scoped_corrections.py::test_segment_revision_undo_restores_raw_text_under_scoped_correction
TEST_CASE("test_segment_revision_undo_restores_raw_text_under_scoped_correction",
          "[test_meeting_scoped_corrections]") {
    Fixture f;
    f.repo->add_segments({make_segment(f.meeting_id, "sg_one", 1, 3, "word word")});
    f.open(true);
    const auto scoped = f.host(scoped_op("sg_one", 0, "fixed", "word", "word word"));
    REQUIRE(scoped.ok);
    CHECK(f.segment("sg_one")["text"] == "fixed word");
    // A trusted system revision stays undoable while agent polish is blocked.
    const auto revision =
        f.store->apply("system", std::string("test"),
                       Json::parse(R"([{"op": "revise_segment_text", "segment_id": "sg_one",
                         "text": "changed word", "evidence": ["sg_one"]}])"))[0];
    REQUIRE(revision.ok);
    CHECK(f.segment("sg_one")["original_text"] == "changed word");
    CHECK(f.store->undo(*revision.seq, "host")[0].ok);
    CHECK(f.segment("sg_one")["original_text"] == "word word");
    CHECK(f.segment("sg_one")["text"] == "fixed word");
    CHECK(f.store->undo(*scoped.seq, "host")[0].ok);
    CHECK(f.segment("sg_one")["text"] == "word word");
}

// from
// tests/test_meeting_scoped_corrections.py::test_segment_revision_preview_keeps_raw_and_displayed_text_in_sync
TEST_CASE("test_segment_revision_preview_keeps_raw_and_displayed_text_in_sync",
          "[test_meeting_scoped_corrections]") {
    class Projected final : public StubRepository {
    public:
        std::optional<Json> get_segment(std::string_view, std::string_view segment_id) override {
            return Json{{"id", segment_id}, {"text", "fixed word"}, {"original_text", "word word"}};
        }
    } repository;
    OpResult result;
    result.ok = true;
    result.op = {{"op", "revise_segment_text"}};
    result.effect = {
        {"entity", "segment_text"}, {"segment_id", "sg_one"}, {"text", "changed word"}};
    const Json inverse = handle_segment_op(repository, "m_one", result);
    CHECK(inverse["text"] == "word word");
    CHECK(result.effect["segment"]["original_text"] == "changed word");
}

// from
// tests/test_meeting_scoped_corrections.py::test_agent_polish_cannot_bake_a_scoped_correction_into_raw_text
TEST_CASE("test_agent_polish_cannot_bake_a_scoped_correction_into_raw_text",
          "[test_meeting_scoped_corrections]") {
    Fixture f;
    f.repo->add_segments({make_segment(f.meeting_id, "sg_one", 1, 3, "word word")});
    f.open(true);
    const auto scoped = f.host(scoped_op("sg_one", 0, "fixed", "word", "word word"));
    REQUIRE(scoped.ok);
    const Json polish = Json::parse(R"({"op": "revise_segment_text", "segment_id": "sg_one",
        "text": "fixed word,", "evidence": ["sg_one"]})");
    const auto blocked = f.store->apply("agent", std::string("polish"), Json::array({polish}))[0];
    CHECK_FALSE(blocked.ok);
    CHECK(blocked.reason == "human_scoped_correction");
    CHECK(f.segment("sg_one")["original_text"] == "word word");
    CHECK(f.store->undo(*scoped.seq, "host")[0].ok);
    CHECK(f.segment("sg_one")["text"] == "word word");
    const auto accepted = f.store->apply("agent", std::string("polish"), Json::array({polish}))[0];
    CHECK(accepted.ok);
    CHECK(f.segment("sg_one")["original_text"] == "fixed word,");
}

// from
// tests/test_meeting_scoped_corrections.py::test_selection_changed_before_save_is_rejected_without_persisting_note
TEST_CASE("test_selection_changed_before_save_is_rejected_without_persisting_note",
          "[test_meeting_scoped_corrections]") {
    Fixture f;
    f.repo->add_segments({make_segment(f.meeting_id, "sg_one", 1, 3, "word word")});
    f.open();
    const Json selected_before_revision = scoped_op("sg_one", 0, "fixed", "word", "word word");
    f.set_raw_text("sg_one", "new word word");
    const auto rejected = f.host(selected_before_revision);
    CHECK_FALSE(rejected.ok);
    CHECK(rejected.reason == "stale_selection");
    CHECK_FALSE(rejected.seq.has_value());
    CHECK(f.store->snapshot()["cards"]["user_notes"] == Json::array());
    CHECK(f.segment("sg_one")["text"] == "new word word");
    const auto retry = f.host(scoped_op("sg_one", 0, "fixed", "word", "new word word"));
    CHECK(retry.ok);
    CHECK(f.segment("sg_one")["text"] == "new fixed word");
}

// from
// tests/test_meeting_scoped_corrections.py::test_selection_fingerprint_uses_current_meeting_wide_corrections
TEST_CASE("test_selection_fingerprint_uses_current_meeting_wide_corrections",
          "[test_meeting_scoped_corrections]") {
    Fixture f;
    f.repo->add_segments({make_segment(f.meeting_id, "sg_one", 1, 3, "word word")});
    f.open();
    REQUIRE(f.host(global_note("Global spelling", "word", "term")).ok);
    CHECK(f.host(scoped_op("sg_one", 0, "fixed", "term", "word word")).reason == "stale_selection");
    CHECK(f.host(scoped_op("sg_one", 0, "fixed", "term", "term term")).ok);
    CHECK(f.segment("sg_one")["text"] == "fixed term");
}
