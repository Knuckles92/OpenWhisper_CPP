// Read-time transcript corrections through the SQL repository
// (tests/test_meeting_corrections.py::TestRepositoryReadTimeCorrection).

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>

#include "meeting_state/corrections.h"
#include "meeting_state/store.h"
#include "sql_support.h"

using fv::Json;
using namespace fv::meeting_state;
using namespace fv::meeting_state::test_support;

namespace {

Json note(const std::string& selected, const std::string& replacement,
          const std::string& status = "edited") {
    return {
        {"id", "it_" + selected},
        {"card", "user_notes"},
        {"status", status},
        {"author_type", "user"},
        {"text", "Correction: " + selected + " -> " + replacement},
        {"data",
         {{"kind", "term_correction"}, {"selected_text", selected}, {"replacement", replacement}}}};
}

Json state(const Json& notes = Json::array()) {
    return {{"cards", {{"user_notes", notes}}}};
}

} // namespace

// from
// tests/test_meeting_corrections.py::TestRepositoryReadTimeCorrection::test_segments_read_corrected_with_raw_text_preserved
TEST_CASE("TestRepositoryReadTimeCorrection::test_segments_read_corrected_with_raw_text_preserved",
          "[test_meeting_corrections]") {
    SqlFixture f;
    const std::string meeting_id = make_meeting(*f.repo);
    f.repo->add_segments({make_segment(meeting_id, "sg_1", 1.0, 2.0, "Entropic ships Claude"),
                          make_segment(meeting_id, "sg_2", 3.0, 4.0, "entropically speaking")});
    f.repo->persist_state(meeting_id, state(Json::array({note("Entropic", "Anthropic")})));

    std::map<std::string, Json> rows;
    for (const auto& row : f.repo->get_segments(meeting_id)) {
        rows[row["id"].get<std::string>()] = row;
    }
    CHECK(rows["sg_1"]["text"] == "Anthropic ships Claude");
    CHECK(rows["sg_1"]["original_text"] == "Entropic ships Claude");
    CHECK(rows["sg_2"]["text"] == "entropically speaking");
    CHECK(f.repo->get_segment(meeting_id, "sg_1")->at("text") == "Anthropic ships Claude");

    // Removing the correction restores the raw words: nothing was rewritten.
    f.repo->persist_state(meeting_id,
                          state(Json::array({note("Entropic", "Anthropic", "removed")})));
    CHECK(f.repo->get_segment(meeting_id, "sg_1")->at("text") == "Entropic ships Claude");
}

// from
// tests/test_meeting_corrections.py::TestRepositoryReadTimeCorrection::test_repository_provider_tracks_state_changes_for_recovery
TEST_CASE(
    "TestRepositoryReadTimeCorrection::test_repository_provider_tracks_state_changes_for_recovery",
    "[test_meeting_corrections]") {
    SqlFixture f;
    const std::string meeting_id = make_meeting(*f.repo);
    const auto provider = repository_term_rules(f.repo, meeting_id);
    const auto as_json = f.repo->term_rules_provider(meeting_id);
    CHECK(provider().empty());
    CHECK(as_json() == Json::object());
    Json corrected = state(Json::array({note("Entropic", "Anthropic")}));
    corrected["seq"] = 2;
    f.repo->persist_state(meeting_id, corrected);
    CHECK(provider() == TermRules{{"entropic", "Anthropic"}});
    CHECK(as_json() == Json({{"entropic", "Anthropic"}}));
    Json cleared = state();
    cleared["seq"] = 3;
    f.repo->persist_state(meeting_id, cleared);
    CHECK(provider().empty());
    CHECK(as_json() == Json::object());
    // A repository that cannot answer gives no rules.
    CHECK(repository_term_rules(nullptr, meeting_id)().empty());
    CHECK(repository_term_rules(f.repo, "m_unknown")().empty());
}

// from
// tests/test_meeting_corrections.py::TestRepositoryReadTimeCorrection::test_other_meetings_are_unaffected
TEST_CASE("TestRepositoryReadTimeCorrection::test_other_meetings_are_unaffected",
          "[test_meeting_corrections]") {
    SqlFixture f;
    const std::string first = make_meeting(*f.repo, "m_a");
    const std::string second = make_meeting(*f.repo, "m_b");
    f.repo->add_segments({make_segment(first, "sg_a", 1.0, 3.0, "Entropic"),
                          make_segment(second, "sg_b", 1.0, 3.0, "Entropic")});
    f.repo->persist_state(first, state(Json::array({note("Entropic", "Anthropic")})));
    CHECK(f.repo->get_segment(first, "sg_a")->at("text") == "Anthropic");
    CHECK(f.repo->get_segment(second, "sg_b")->at("text") == "Entropic");
}

TEST_CASE("a stale one-occurrence correction is refused and writes nothing",
          "[meeting_state_store]") {
    SqlFixture f;
    const std::string meeting_id = make_meeting(*f.repo);
    f.repo->add_segments({make_segment(meeting_id, "sg_1", 1.0, 3.0, "Friday or Friday")});
    MeetingStateStore store(
        state_for(meeting_id), f.repo, {}, f.clock, f.ids,
        {[&](std::string_view id) { return f.repo->segment_exists(meeting_id, id); }, {}, {}});
    const auto op = [&](const std::string& fingerprint) {
        return Json::array({{{"op", "add_item"},
                             {"card", "user_notes"},
                             {"text", "fix"},
                             {"evidence", {"sg_1"}},
                             {"data",
                              {{"kind", "occurrence_correction"},
                               {"selected_text", "Friday"},
                               {"replacement", "Thursday"},
                               {"occurrence_index", 1},
                               {"base_fingerprint", fingerprint}}}}});
    };
    const auto stale =
        store.apply("host", "p_host", op(segment_fingerprint("Friday or Monday")))[0];
    CHECK_FALSE(stale.ok);
    CHECK(stale.reason == "stale_selection");
    CHECK(f.repo->list_events(meeting_id).empty());
    CHECK(store.seq() == 0);

    const auto fresh =
        store.apply("host", "p_host", op(segment_fingerprint("Friday or Friday")))[0];
    CHECK(fresh.ok);
    CHECK(f.repo->get_segment(meeting_id, "sg_1")->at("text") == "Friday or Thursday");
    CHECK(f.repo->get_segment(meeting_id, "sg_1")->at("original_text") == "Friday or Friday");
}
