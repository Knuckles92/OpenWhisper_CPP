// tests/test_meeting_corrections.py: meeting-wide term corrections and the vocabulary hint.
// The repository read-time tests are M7.7's, the guidance prompt M8's.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "meeting_state/corrections.h"

using fv::Json;
using namespace fv::meeting_state;

namespace {

Json note(const Json& selected, const std::string& replacement,
          const std::string& kind = "term_correction", const Json& overrides = Json::object()) {
    Json item = {
        {"id", "it_" + (selected.is_string() ? selected.get<std::string>() : "x")},
        {"card", "user_notes"},
        {"status", "edited"},
        {"author_type", "user"},
        {"text", "Correction"},
        {"data", {{"kind", kind}, {"selected_text", selected}, {"replacement", replacement}}}};
    item.update(overrides);
    return item;
}

Json state_of(const Json& notes) {
    return {{"cards", {{"user_notes", notes}}}};
}

TermRules rules(std::initializer_list<std::pair<std::string, std::string>> pairs) {
    return TermRules(pairs.begin(), pairs.end());
}

} // namespace

// from tests/test_meeting_corrections.py::TestTermRules::
//     test_only_live_human_term_corrections_within_bounds_qualify
TEST_CASE("TestTermRules::test_only_live_human_term_corrections_within_bounds_qualify",
          "[test_meeting_corrections]") {
    const TermRules found = term_rules(state_of(Json::array({
        note("Entropic", "Anthropic"),
        note("gone", "Gone", "term_correction", {{"status", "removed"}}),
        note("agent", "Agent", "term_correction", {{"author_type", "agent"}}),
        note("insight", "Insight", "agent_insight"),
        note(std::string(121, 'x'), "Long"),
        note("blank", "   "),
        Json{{"text", "plain user note"}, {"status", "edited"}, {"author_type", "user"}},
    })));
    CHECK(found == rules({{"entropic", "Anthropic"}}));
}

// from tests/test_meeting_corrections.py::TestTermRules::
//     test_later_note_wins_and_items_helper_matches_state_helper
TEST_CASE("TestTermRules::test_later_note_wins_and_items_helper_matches_state_helper",
          "[test_meeting_corrections]") {
    const Json notes = Json::array({note("Entropic", "Anthropic"), note("entropic", "Acme")});
    CHECK(term_rules(state_of(notes)) == rules({{"entropic", "Acme"}}));
    CHECK(term_rules_from_items(notes) == term_rules(state_of(notes)));
}

// from tests/test_meeting_corrections.py::TestTermRules::test_empty_state_shapes
TEST_CASE("TestTermRules::test_empty_state_shapes", "[test_meeting_corrections]") {
    CHECK(term_rules(Json::object()).empty());
    CHECK(term_rules(Json{{"cards", nullptr}}).empty());
    CHECK(term_rules(state_of(Json::array({nullptr, "junk"}))).empty());
}

// from tests/test_meeting_corrections.py::TestCorrectText::
//     test_whole_word_case_insensitive_replacement
TEST_CASE("TestCorrectText::test_whole_word_case_insensitive_replacement",
          "[test_meeting_corrections]") {
    const TermRules entropic = rules({{"entropic", "Anthropic"}});
    CHECK(correct_text("Entropic and ENTROPIC, entropic.", entropic) ==
          "Anthropic and Anthropic, Anthropic.");
    CHECK(correct_text("entropically unentropic", entropic) == "entropically unentropic");
}

// from tests/test_meeting_corrections.py::TestCorrectText::
//     test_single_pass_prevents_chaining_and_prefers_longest_match
TEST_CASE("TestCorrectText::test_single_pass_prevents_chaining_and_prefers_longest_match",
          "[test_meeting_corrections]") {
    // Python's test names its own product; the phrase here is this product's.
    const TermRules chained = rules({{"a", "b"},
                                     {"b", "c"},
                                     {"open whisper c p p", "OpenWhisper_CPP"},
                                     {"whisper", "Whisper"}});
    CHECK(correct_text("a b", chained) == "b c");
    CHECK(correct_text("open whisper c p p uses whisper", chained) ==
          "OpenWhisper_CPP uses Whisper");
}

// from tests/test_meeting_corrections.py::TestCorrectText::test_regex_syntax_in_terms_is_literal
TEST_CASE("TestCorrectText::test_regex_syntax_in_terms_is_literal", "[test_meeting_corrections]") {
    CHECK(correct_text("we use c++ (fast)", rules({{"c++", "C++"}, {"(fast)", "quick"}})) ==
          "we use C++ quick");
}

// from tests/test_meeting_corrections.py::TestCorrectText::test_no_rules_or_empty_text_is_identity
TEST_CASE("TestCorrectText::test_no_rules_or_empty_text_is_identity",
          "[test_meeting_corrections]") {
    CHECK(correct_text("Entropic", {}) == "Entropic");
    CHECK(correct_text("", rules({{"entropic", "Anthropic"}})).empty());
}

// from
// tests/test_meeting_corrections.py::TestVocabulary::test_terms_are_distinct_ordered_and_bounded
TEST_CASE("TestVocabulary::test_terms_are_distinct_ordered_and_bounded",
          "[test_meeting_corrections]") {
    const TermRules three =
        rules({{"entropic", "Anthropic"}, {"antropic", "anthropic"}, {"cloud", "Claude"}});
    CHECK(vocabulary_terms(three) == std::vector<std::string>{"Anthropic", "Claude"});
    CHECK(vocabulary_hint(three) == "Anthropic, Claude.");
    CHECK(vocabulary_hint({}).empty());
    TermRules many;
    for (int i = 0; i < 40; ++i) {
        many.emplace_back(std::to_string(i), "Term" + std::to_string(i));
    }
    CHECK(vocabulary_terms(many).size() == 12);
    const auto bounded =
        vocabulary_terms(rules({{"a", std::string(100, 'x')}, {"b", std::string(100, 'y')}}));
    std::string joined;
    for (const auto& term : bounded) {
        joined += (joined.empty() ? "" : ", ") + term;
    }
    CHECK(joined.size() <= 160);
}

TEST_CASE("one-occurrence corrections apply to their cited passage only",
          "[test_meeting_corrections]") {
    const std::string base = "Entropic ships Entropic";
    const Json item = {{"id", "it_o"},
                       {"card", "user_notes"},
                       {"status", "edited"},
                       {"author_type", "user"},
                       {"evidence", {"sg_1"}},
                       {"data",
                        {{"kind", "occurrence_correction"},
                         {"selected_text", " Entropic "},
                         {"replacement", "Acme"},
                         {"occurrence_index", 1},
                         {"base_fingerprint", segment_fingerprint(base)}}}};
    const ScopedRules scoped = occurrence_rules(state_of(Json::array({item})));
    REQUIRE(scoped.contains("sg_1"));
    CHECK(scoped.at("sg_1").front().selected_text == "Entropic");
    CHECK(correct_segment_text(base, "sg_1", {}, scoped) == "Entropic ships Acme");
    CHECK(correct_segment_text(base, "sg_2", {}, scoped) == base);
    // A meeting-wide rule changes the base text, which suspends the scoped edit.
    CHECK(correct_segment_text(base, "sg_1", rules({{"ships", "sails"}}), scoped) ==
          "Entropic sails Entropic");
    CHECK(segment_fingerprint("") == "fnv1a64:cbf29ce484222325");
    CHECK(valid_segment_fingerprint(segment_fingerprint("x")));
    CHECK_FALSE(valid_segment_fingerprint("fnv1a64:CBF29CE484222325"));
}
