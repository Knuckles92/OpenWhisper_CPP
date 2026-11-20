// tests/test_personalize_s4_dictionary.py: the personal dictionary's storage, hints,
// replacements, prompt and spelling rules; plus the services.dictionary golden vectors.
//
// Python monkeypatches config.MAX_DICTIONARY_TERMS, MAX_RECOGNITION_PHRASES, PROMPT_MAX_TERMS and
// PROMPT_MAX_CHARS to small numbers; the C++ limits are constants, so these tests build inputs
// past the real limits instead.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/json.h"
#include "golden/golden.h"
#include "settings/store.h"
#include "testkit/testkit.h"
#include "text/dictionary.h"
#include "text/vocabulary.h"
#include "text_test_support.h"

using Catch::Matchers::ContainsSubstring;
using fv::Json;
using fv::text::DictionaryError;
using fv::text::DictionaryTerm;
using fv::text_test::dictionary_settings;
using fv::text_test::entry;
using fv::text_test::kDictionary;
using fv::text_test::kRules;
using fv::text_test::plain;
using fv::text_test::term;
using fv::text_test::terms_of;
namespace text = fv::text;
namespace golden = fv::golden;

namespace {

Json words(int count, int first = 0) {
    Json list = Json::array();
    for (int i = first; i < first + count; ++i) {
        list.push_back(entry("Word" + std::to_string(i)));
    }
    return list;
}

std::vector<std::string> strings(const Json& list) {
    return list.get<std::vector<std::string>>();
}

// The case file's mask: an id a mutator made at random (twelve lowercase hex digits no input
// entry had) reads "$new".
Json mask(const Json& value, const std::set<std::string>& known) {
    static const std::regex fresh("[0-9a-f]{12}");
    if (value.is_object()) {
        Json out = Json::object();
        for (const auto& [key, item] : value.items()) {
            if (key == "id" && item.is_string() && !known.contains(item.get<std::string>()) &&
                std::regex_match(item.get<std::string>(), fresh)) {
                out[key] = "$new";
            } else {
                out[key] = mask(item, known);
            }
        }
        return out;
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const Json& item : value) {
            out.push_back(mask(item, known));
        }
        return out;
    }
    return value;
}

std::set<std::string> input_ids(const Json& settings) {
    std::set<std::string> ids;
    if (settings.is_object() && settings.contains(kDictionary) &&
        settings.at(kDictionary).is_array()) {
        for (const Json& item : settings.at(kDictionary)) {
            if (item.is_object() && item.contains("id") && item.at("id").is_string()) {
                ids.insert(item.at("id").get<std::string>());
            }
        }
    }
    return ids;
}

// Runs a mutator as the case file does: on a copy, {"result", "settings"} or {"error"}.
template <class Mutate> Json run_mutator(const Json& settings, Mutate mutate) {
    Json changed = settings;
    Json out = Json::object();
    try {
        out["result"] = mutate(changed);
    } catch (const DictionaryError& error) {
        return {{"error", error.what()}};
    }
    out["settings"] = changed;
    return mask(out, input_ids(settings));
}

std::vector<std::string> heard_arg(const golden::Case& c, std::size_t index,
                                   std::string_view keyword) {
    if (c.arg_count() > index) {
        return c.arg<std::vector<std::string>>(index);
    }
    const std::string key(keyword);
    if (c.kwargs().contains(key)) {
        return c.kwargs().at(key).get<std::vector<std::string>>();
    }
    return {};
}

bool flag_arg(const golden::Case& c, std::size_t index, std::string_view keyword) {
    if (c.arg_count() > index) {
        return c.arg<bool>(index);
    }
    const std::string key(keyword);
    return c.kwargs().contains(key) && c.kwargs().at(key).get<bool>();
}

void check_mutator(std::string_view function,
                   const std::function<Json(const golden::Case&, Json&)>& mutate) {
    golden::for_each("services.dictionary", function, [&](const golden::Case& c) {
        const Json got =
            run_mutator(c.args().at(0), [&](Json& settings) { return mutate(c, settings); });
        CHECK(golden::equal(c.result<Json>(), got));
    });
}

} // namespace

// ---- TestLoading ------------------------------------------------------------------------------

// from tests/test_personalize_s4_dictionary.py::TestLoading::test_tolerates_malformed_values
TEST_CASE("TestLoading::test_tolerates_malformed_values", "[test_personalize_s4_dictionary]") {
    CHECK(text::load_dictionary(Json::object()).empty());
    CHECK(text::load_dictionary(dictionary_settings("Kubernetes")).empty());
    const Json settings = dictionary_settings(Json::array({
        "loose string",
        nullptr,
        7,
        {{"term", ""}},
        {{"term", "   "}},
        {{"term", 5}},
        {{"term", std::string(65, 'x')}},
        entry("Ksenia", {{"starred", "yes"}, {"learned", 1}, {"heard", "Sonia"}}),
    }));
    CHECK(text::load_dictionary(settings) ==
          std::vector<DictionaryTerm>{term("ksenia", "Ksenia", {{"heard", {"Sonia"}}})});
}

// from
// tests/test_personalize_s4_dictionary.py::TestLoading::test_dedupes_ignoring_case_and_keeps_the_first
TEST_CASE("TestLoading::test_dedupes_ignoring_case_and_keeps_the_first",
          "[test_personalize_s4_dictionary]") {
    const Json settings = dictionary_settings(Json::array({
        entry("OpenWhisper", {{"starred", true}}),
        entry("openwhisper", {{"id", "b"}}),
        entry("Kubernetes"),
    }));
    CHECK(terms_of(settings) == std::vector<std::string>{"OpenWhisper", "Kubernetes"});
    CHECK(text::load_dictionary(settings)[0].starred);
}

// from
// tests/test_personalize_s4_dictionary.py::TestLoading::test_collapses_whitespace_and_gives_stable_ids
TEST_CASE("TestLoading::test_collapses_whitespace_and_gives_stable_ids",
          "[test_personalize_s4_dictionary]") {
    const Json settings = dictionary_settings(Json::array({
        {{"term", "  Alex   Rivera "}},
        {{"term", "Ksenia"}, {"id", ""}},
    }));
    const auto first = text::load_dictionary(settings);
    CHECK(terms_of(settings) == std::vector<std::string>{"Alex Rivera", "Ksenia"});
    CHECK(first == text::load_dictionary(settings));
    REQUIRE(first.size() == 2);
    CHECK(first[0].id != first[1].id);
}

// from tests/test_personalize_s4_dictionary.py::TestLoading::test_duplicate_ids_are_made_unique
TEST_CASE("TestLoading::test_duplicate_ids_are_made_unique", "[test_personalize_s4_dictionary]") {
    const Json settings = dictionary_settings(Json::array({
        entry("One", {{"id", "same"}}),
        entry("Two", {{"id", "same"}}),
    }));
    const auto loaded = text::load_dictionary(settings);
    REQUIRE(loaded.size() == 2);
    CHECK(loaded[0].id == "same");
    CHECK(loaded[1].id != "same");
}

// from tests/test_personalize_s4_dictionary.py::TestLoading::test_caps_terms_and_heard_variants
TEST_CASE("TestLoading::test_caps_terms_and_heard_variants", "[test_personalize_s4_dictionary]") {
    const auto loaded = text::load_dictionary(dictionary_settings(words(text::MAX_TERMS + 2)));
    REQUIRE(loaded.size() == text::MAX_TERMS);
    CHECK(loaded.front().term == "Word0");
    CHECK(loaded.back().term == "Word499");
    const Json heard = {"a1", "a2", "A1", "Word0", "a3", "a4", "a5", "a6", std::string(65, 'x')};
    const auto capped = text::load_dictionary(
        dictionary_settings(Json::array({entry("Word0", {{"heard", heard}})})));
    CHECK(capped[0].heard == std::vector<std::string>{"a1", "a2", "a3", "a4", "a5"});
}

// from
// tests/test_personalize_s4_dictionary.py::TestLoading::test_learned_terms_never_keep_heard_variants_and_only_they_are_new
TEST_CASE("TestLoading::test_learned_terms_never_keep_heard_variants_and_only_they_are_new",
          "[test_personalize_s4_dictionary]") {
    const Json settings = dictionary_settings(Json::array({
        entry("Ksenia", {{"learned", true}, {"new", true}, {"heard", {"sonia"}}}),
        entry("Kubernetes", {{"new", true}}),
    }));
    const auto loaded = text::load_dictionary(settings);
    REQUIRE(loaded.size() == 2);
    const DictionaryTerm& learned = loaded[0];
    const DictionaryTerm& typed = loaded[1];
    CHECK((learned.learned && learned.is_new && learned.heard.empty()));
    CHECK_FALSE(typed.is_new);
}

// ---- TestRecognitionPhrases -------------------------------------------------------------------

// from
// tests/test_personalize_s4_dictionary.py::TestRecognitionPhrases::test_starred_first_then_saved_order
TEST_CASE("TestRecognitionPhrases::test_starred_first_then_saved_order",
          "[test_personalize_s4_dictionary]") {
    const Json settings = dictionary_settings(Json::array({
        entry("Alpha"),
        entry("Bravo", {{"starred", true}}),
        entry("Charlie"),
        entry("Delta", {{"starred", true}, {"learned", true}}),
    }));
    CHECK(text::recognition_phrases(settings) ==
          std::vector<std::string>{"Bravo", "Delta", "Alpha", "Charlie"});
}

// from
// tests/test_personalize_s4_dictionary.py::TestRecognitionPhrases::test_budget_keeps_the_starred_terms
TEST_CASE("TestRecognitionPhrases::test_budget_keeps_the_starred_terms",
          "[test_personalize_s4_dictionary]") {
    Json list = words(text::MAX_RECOGNITION_PHRASES + 1);
    list.push_back(entry("Charlie", {{"starred", true}}));
    const auto phrases = text::recognition_phrases(dictionary_settings(list));
    REQUIRE(phrases.size() == text::MAX_RECOGNITION_PHRASES);
    CHECK(phrases.front() == "Charlie");
    CHECK(phrases[1] == "Word0");
    CHECK(phrases.back() == "Word48");
}

// from
// tests/test_personalize_s4_dictionary.py::TestRecognitionPhrases::test_off_when_steering_is_off
TEST_CASE("TestRecognitionPhrases::test_off_when_steering_is_off",
          "[test_personalize_s4_dictionary]") {
    Json settings = dictionary_settings(Json::array({entry("Alpha")}));
    settings["dictionary_steer_recognition"] = false;
    CHECK(text::recognition_phrases(settings).empty());
    CHECK(text::recognition_phrases(Json::object()).empty());
}

// ---- TestReplacements -------------------------------------------------------------------------

// from tests/test_personalize_s4_dictionary.py::TestReplacements::test_whole_words_any_case
TEST_CASE("TestReplacements::test_whole_words_any_case", "[test_personalize_s4_dictionary]") {
    const auto terms = text::load_dictionary(
        dictionary_settings(Json::array({entry("Ksenia", {{"heard", {"sonia", "senya"}}})})));
    CHECK(text::apply_replacements("Sonia and SENYA met Sonias", terms) ==
          "Ksenia and Ksenia met Sonias");
}

// from
// tests/test_personalize_s4_dictionary.py::TestReplacements::test_longest_variant_wins_and_nothing_chains
TEST_CASE("TestReplacements::test_longest_variant_wins_and_nothing_chains",
          "[test_personalize_s4_dictionary]") {
    const auto terms = text::load_dictionary(dictionary_settings(Json::array({
        entry("Kubernetes", {{"heard", {"cooper", "cooper netties"}}}),
        entry("Netty", {{"heard", {"Kubernetes"}}}),
    })));
    CHECK(text::apply_replacements("ship cooper netties and cooper", terms) ==
          "ship Kubernetes and Kubernetes");
}

// from tests/test_personalize_s4_dictionary.py::TestReplacements::test_learned_terms_never_replace
TEST_CASE("TestReplacements::test_learned_terms_never_replace",
          "[test_personalize_s4_dictionary]") {
    const std::vector<DictionaryTerm> terms = {
        term("a", "Ksenia", {{"heard", {"sonia"}}, {"learned", true}})};
    CHECK(text::apply_replacements("Sonia called", terms) == "Sonia called");
    CHECK(text::apply_replacements("", terms).empty());
}

// from
// tests/test_personalize_s4_dictionary.py::TestReplacements::test_starred_term_wins_a_shared_variant
TEST_CASE("TestReplacements::test_starred_term_wins_a_shared_variant",
          "[test_personalize_s4_dictionary]") {
    const auto terms = text::load_dictionary(dictionary_settings(Json::array({
        entry("Jon", {{"heard", {"john"}}}),
        entry("Jonn", {{"heard", {"john"}}, {"starred", true}}),
    })));
    CHECK(text::apply_replacements("john", terms) == "Jonn");
}

// from
// tests/test_personalize_s4_dictionary.py::TestReplacements::test_meeting_corrections_keep_their_api
TEST_CASE("TestReplacements::test_meeting_corrections_keep_their_api",
          "[test_personalize_s4_dictionary]") {
    // meeting.corrections.correct_text is replace_terms over its rules (fv_meeting_state).
    CHECK(text::replace_terms("the jon file", {{"jon", "John"}}) == "the John file");
    CHECK(text::replace_terms("", {{"jon", "John"}}).empty());
}

TEST_CASE("a heard variant typed in another case reaches the transcript", "[dictionary]") {
    // Demo step 2: "Ksenia" sounds like "Ksenya".
    Json settings = Json::object();
    text::add_term(settings, "Ksenia", {"Ksenya"});
    CHECK(text::apply_replacements("ksenya came", text::load_dictionary(settings)) ==
          "Ksenia came");
}

// ---- TestPromptBlock --------------------------------------------------------------------------

// from tests/test_personalize_s4_dictionary.py::TestPromptBlock::test_lists_starred_first
TEST_CASE("TestPromptBlock::test_lists_starred_first", "[test_personalize_s4_dictionary]") {
    const auto terms = text::load_dictionary(
        dictionary_settings(Json::array({entry("Alpha"), entry("Bravo", {{"starred", true}})})));
    CHECK(text::prompt_block(terms) ==
          "Vocabulary — when one of these is what was said, including near-miss "
          "mishearings, spell it exactly: Bravo, Alpha.");
    CHECK(text::prompt_block({}).empty());
}

// from tests/test_personalize_s4_dictionary.py::TestPromptBlock::test_is_budgeted
TEST_CASE("TestPromptBlock::test_is_budgeted", "[test_personalize_s4_dictionary]") {
    // PROMPT_MAX_TERMS: 205 words list the first 200.
    const auto many = text::load_dictionary(dictionary_settings(words(205)));
    CHECK_THAT(text::prompt_block(many), Catch::Matchers::EndsWith("Word198, Word199."));
    // PROMPT_MAX_CHARS: 62-character words cost 64 each, so 37 fit in 2400.
    Json long_words = Json::array();
    for (int i = 0; i < 60; ++i) {
        long_words.push_back(entry(std::string(56, 'w') + "Word" + std::to_string(i + 10)));
    }
    const std::string block =
        text::prompt_block(text::load_dictionary(dictionary_settings(long_words)));
    CHECK_THAT(block, Catch::Matchers::EndsWith("Word46."));
    CHECK_THAT(block, !ContainsSubstring("Word47"));
}

// ---- TestMutators -----------------------------------------------------------------------------

// from tests/test_personalize_s4_dictionary.py::TestMutators::test_add_puts_new_words_on_top
TEST_CASE("TestMutators::test_add_puts_new_words_on_top", "[test_personalize_s4_dictionary]") {
    Json settings = Json::object();
    text::add_term(settings, "Kubernetes", {"cooper netties"});
    const DictionaryTerm added = text::add_term(settings, "  Ksenia ", {}, true);
    CHECK(terms_of(settings) == std::vector<std::string>{"Ksenia", "Kubernetes"});
    CHECK(added.starred);
    CHECK_FALSE(added.id.empty());
    const Json& saved = settings.at(kDictionary).at(1);
    const Json expected = {{"id", saved.at("id")}, {"term", "Kubernetes"},
                           {"starred", false},     {"heard", {"cooper netties"}},
                           {"learned", false},     {"new", false}};
    CHECK(saved == expected);
}

// from tests/test_personalize_s4_dictionary.py::TestMutators::test_add_rejects_bad_input
TEST_CASE("TestMutators::test_add_rejects_bad_input", "[test_personalize_s4_dictionary]") {
    struct Row {
        std::string term;
        std::vector<std::string> heard;
        std::string message;
    };
    const auto row = GENERATE(values<Row>({
        {"", {}, "Type a word"},
        {std::string(65, 'x'), {}, "under 64"},
        {"Word", {std::string(65, 'y')}, "under 64"},
        {"Word", {"a", "b", "c", "d", "e", "f"}, "Up to 5"},
    }));
    Json settings = Json::object();
    CHECK_THROWS_WITH(text::add_term(settings, row.term, row.heard),
                      ContainsSubstring(row.message));
    CHECK(settings == Json::object());
}

// from
// tests/test_personalize_s4_dictionary.py::TestMutators::test_re_adding_merges_and_confirms_a_learned_word
TEST_CASE("TestMutators::test_re_adding_merges_and_confirms_a_learned_word",
          "[test_personalize_s4_dictionary]") {
    Json settings =
        dictionary_settings(Json::array({entry("ksenia", {{"learned", true}, {"new", true}})}));
    const DictionaryTerm merged = text::add_term(settings, "Ksenia", {"sonia"});
    CHECK(merged.term == "Ksenia");
    CHECK(merged.heard == std::vector<std::string>{"sonia"});
    CHECK_FALSE(merged.learned);
    CHECK_FALSE(merged.is_new);
    CHECK_THROWS_AS(text::add_term(settings, "Ksenia", {"Sonia"}), DictionaryError);
    CHECK_THROWS_WITH(text::add_term(settings, "Ksenia", {"Sonia"}), ContainsSubstring("already"));
    CHECK(settings.at(kDictionary).size() == 1);
}

// from tests/test_personalize_s4_dictionary.py::TestMutators::test_full_dictionary_refuses
TEST_CASE("TestMutators::test_full_dictionary_refuses", "[test_personalize_s4_dictionary]") {
    Json settings = dictionary_settings(words(text::MAX_TERMS - 1));
    text::add_term(settings, "One");
    CHECK_THROWS_WITH(text::add_term(settings, "Two"), ContainsSubstring("full"));
    CHECK_THROWS_WITH(text::add_term(settings, "Two"), "Your dictionary is full (500 words).");
}

// from tests/test_personalize_s4_dictionary.py::TestMutators::test_update_star_delete
TEST_CASE("TestMutators::test_update_star_delete", "[test_personalize_s4_dictionary]") {
    Json settings = Json::object();
    const DictionaryTerm word = text::add_term(settings, "Kubernetes");
    const DictionaryTerm other = text::add_term(settings, "Ksenia");
    CHECK_THROWS_WITH(text::update_term(settings, word.id, "ksenia", {}),
                      ContainsSubstring("already"));
    const DictionaryTerm updated = text::update_term(settings, word.id, "K8s", {"kates"});
    CHECK(updated.id == word.id);
    CHECK(updated.term == "K8s");
    CHECK(updated.heard == std::vector<std::string>{"kates"});
    CHECK(text::set_starred(settings, other.id, true).starred);
    const auto removed = text::delete_term(settings, word.id);
    REQUIRE(removed);
    CHECK(removed->term == "K8s");
    CHECK_FALSE(text::delete_term(settings, word.id));
    CHECK(terms_of(settings) == std::vector<std::string>{"Ksenia"});
    CHECK_THROWS_WITH(text::set_starred(settings, "missing", true), ContainsSubstring("no longer"));
}

// from
// tests/test_personalize_s4_dictionary.py::TestMutators::test_editing_or_starring_a_learned_word_reviews_it
TEST_CASE("TestMutators::test_editing_or_starring_a_learned_word_reviews_it",
          "[test_personalize_s4_dictionary]") {
    Json settings = dictionary_settings(Json::array({
        entry("Ksenia", {{"learned", true}, {"new", true}}),
        entry("Olu", {{"learned", true}, {"new", true}}),
    }));
    const DictionaryTerm starred = text::set_starred(settings, "ksenia", true);
    CHECK(starred.learned);
    CHECK_FALSE(starred.is_new);
    const DictionaryTerm edited = text::update_term(settings, "olu", "Olúwasẹ́un", {"olu"});
    CHECK_FALSE(edited.learned);
    CHECK_FALSE(edited.is_new);
}

// from
// tests/test_personalize_s4_dictionary.py::TestMutators::test_undo_learned_only_removes_learned_words
TEST_CASE("TestMutators::test_undo_learned_only_removes_learned_words",
          "[test_personalize_s4_dictionary]") {
    Json settings = dictionary_settings(Json::array({
        entry("Ksenia", {{"learned", true}, {"new", true}}),
        entry("Typed"),
    }));
    CHECK_FALSE(text::undo_learned(settings, "typed"));
    const auto undone = text::undo_learned(settings, "ksenia");
    REQUIRE(undone);
    CHECK(undone->term == "Ksenia");
    CHECK(terms_of(settings) == std::vector<std::string>{"Typed"});
}

// from tests/test_personalize_s4_dictionary.py::TestMutators::test_clear_new
TEST_CASE("TestMutators::test_clear_new", "[test_personalize_s4_dictionary]") {
    Json settings = dictionary_settings(Json::array({
        entry("A", {{"learned", true}, {"new", true}}),
        entry("B", {{"learned", true}, {"new", true}}),
        entry("C"),
    }));
    CHECK(text::clear_new(settings) == 2);
    CHECK(text::clear_new(settings) == 0);
    const auto loaded = text::load_dictionary(settings);
    for (const DictionaryTerm& item : loaded) {
        CHECK_FALSE(item.is_new);
    }
    CHECK(loaded[0].learned);
    CHECK(loaded[1].learned);
}

// from tests/test_personalize_s4_dictionary.py::TestMutators::test_add_learned
TEST_CASE("TestMutators::test_add_learned", "[test_personalize_s4_dictionary]") {
    Json settings = Json::object();
    const auto learned = text::add_learned(settings, "Ksenia");
    REQUIRE(learned);
    CHECK(learned->learned);
    CHECK(learned->is_new);
    CHECK(learned->heard.empty());
    CHECK_FALSE(text::add_learned(settings, "ksenia"));
    Json off = {{"dictionary_learn_enabled", false}};
    CHECK_FALSE(text::add_learned(off, "Olu"));
}

// from tests/test_personalize_s4_dictionary.py::TestMutators::test_summary
TEST_CASE("TestMutators::test_summary", "[test_personalize_s4_dictionary]") {
    CHECK(text::summary(Json::object()) == "No words yet");
    CHECK(text::summary(dictionary_settings(Json::array({entry("A")}))) == "1 word");
    CHECK(text::summary(dictionary_settings(Json::array({
              entry("A"),
              entry("B", {{"learned", true}, {"new", true}}),
          }))) == "2 words · 1 new");
}

// from tests/test_personalize_s4_dictionary.py::TestMutators::test_parse_heard
TEST_CASE("TestMutators::test_parse_heard", "[test_personalize_s4_dictionary]") {
    CHECK(text::parse_heard(" sonia, senya ;  ksen ya,, ") ==
          std::vector<std::string>{"sonia", "senya", "ksen ya"});
    CHECK(text::parse_heard("").empty());
}

// from
// tests/test_personalize_s4_dictionary.py::TestMutators::test_works_through_the_settings_manager
TEST_CASE("TestMutators::test_works_through_the_settings_manager",
          "[test_personalize_s4_dictionary]") {
    const fv::testkit::TempDataRoot root;
    fv::SettingsStore store(root.path() / "settings.json");
    store.mutate([](Json& settings) { text::add_term(settings, "Ksenia"); });
    CHECK(terms_of(*store.snapshot()) == std::vector<std::string>{"Ksenia"});
    // A refused change writes nothing.
    CHECK_THROWS_AS(store.mutate([](Json& settings) { text::add_term(settings, "Ksenia"); }),
                    DictionaryError);
    CHECK(terms_of(*store.snapshot()) == std::vector<std::string>{"Ksenia"});
}

// ---- TestSpellingRules ------------------------------------------------------------------------

// from tests/test_personalize_s4_dictionary.py::TestSpellingRules::test_plain_spelling_rules
TEST_CASE("TestSpellingRules::test_plain_spelling_rules", "[test_personalize_s4_dictionary]") {
    using Expected = std::pair<std::string, std::vector<std::string>>;
    const auto [rule, expected] = GENERATE(values<std::pair<std::string, Expected>>({
        {R"(Always spell my name "Alex Rivera")", {"Alex Rivera", {}}},
        {"Always spell the name “Ksenia” exactly as written.", {"Ksenia", {}}},
        {R"(Spell "jon" as "John".)", {"John", {"jon"}}},
        {R"(Spell it "Ksenia", not "Sonia")", {"Ksenia", {"Sonia"}}},
    }));
    CHECK(text::spelling_rule(rule) == std::optional<Expected>(expected));
}

// from tests/test_personalize_s4_dictionary.py::TestSpellingRules::test_other_rules_stay_rules
TEST_CASE("TestSpellingRules::test_other_rules_stay_rules", "[test_personalize_s4_dictionary]") {
    const std::string rule = GENERATE(values<std::string>({
        "Use British spelling.",
        R"(Write "Kubernetes" in full.)",
        R"(Spell "k8s" as "Kubernetes" except in code blocks.)",
        R"(Spell "a", "b" and "c" carefully.)",
        "Always spell \"" + std::string(70, 'x') + "\"",
    }));
    CHECK_FALSE(text::spelling_rule(rule));
}

// from tests/test_personalize_s4_dictionary.py::TestSpellingRules::test_moving_rules
TEST_CASE("TestSpellingRules::test_moving_rules", "[test_personalize_s4_dictionary]") {
    Json settings = Json::object();
    settings[std::string(kRules)] = {"Use British spelling.", R"(Spell "jon" as "John".)",
                                     R"(Always spell my name "Ksenia")"};
    settings[std::string(kDictionary)] = Json::array({entry("Ksenia")});
    CHECK(
        text::spelling_rules(settings) ==
        std::vector<std::string>{R"(Spell "jon" as "John".)", R"(Always spell my name "Ksenia")"});
    CHECK(text::move_spelling_rules(settings) == 2);
    CHECK(strings(settings.at(kRules)) == std::vector<std::string>{"Use British spelling."});
    CHECK(terms_of(settings) == std::vector<std::string>{"John", "Ksenia"});
    CHECK(text::load_dictionary(settings)[0].heard == std::vector<std::string>{"jon"});
    CHECK(text::move_spelling_rules(settings) == 0);
}

// ---- golden vectors ---------------------------------------------------------------------------

TEST_CASE("golden _parse", "[golden]") {
    golden::for_each("services.dictionary", "_parse", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), plain(text::parse_terms(c.args().at(0)))));
    });
}

TEST_CASE("golden load_dictionary", "[golden]") {
    golden::for_each("services.dictionary", "load_dictionary", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), plain(text::load_dictionary(c.args().at(0)))));
    });
}

TEST_CASE("golden apply_replacements", "[golden]") {
    golden::for_each("services.dictionary", "apply_replacements", [](const golden::Case& c) {
        CHECK(text::apply_replacements(c.arg<std::string>(0), fv::text_test::terms_from(c.args().at(
                                                                  1))) == c.result<std::string>());
    });
}

TEST_CASE("golden recognition_phrases", "[golden]") {
    golden::for_each("services.dictionary", "recognition_phrases", [](const golden::Case& c) {
        CHECK(text::recognition_phrases(c.args().at(0)) == c.result<std::vector<std::string>>());
    });
}

TEST_CASE("golden prompt_block", "[golden]") {
    golden::for_each("services.dictionary", "prompt_block", [](const golden::Case& c) {
        CHECK(text::prompt_block(fv::text_test::terms_from(c.args().at(0))) ==
              c.result<std::string>());
    });
}

TEST_CASE("golden summary", "[golden]") {
    golden::for_each("services.dictionary", "summary", [](const golden::Case& c) {
        CHECK(text::summary(c.args().at(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden parse_heard", "[golden]") {
    golden::for_each("services.dictionary", "parse_heard", [](const golden::Case& c) {
        CHECK(text::parse_heard(c.arg<std::string>(0)) == c.result<std::vector<std::string>>());
    });
}

TEST_CASE("golden spelling_rule", "[golden]") {
    golden::for_each("services.dictionary", "spelling_rule", [](const golden::Case& c) {
        const auto found = text::spelling_rule(c.arg<std::string>(0));
        const Json got = found ? Json::array({found->first, found->second}) : Json(nullptr);
        CHECK(golden::equal(c.result<Json>(), got));
    });
}

TEST_CASE("golden spelling_rules", "[golden]") {
    golden::for_each("services.dictionary", "spelling_rules", [](const golden::Case& c) {
        CHECK(text::spelling_rules(c.args().at(0)) == c.result<std::vector<std::string>>());
    });
}

TEST_CASE("golden find_term", "[golden]") {
    golden::for_each("services.dictionary", "find_term", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(),
                            plain(text::find_term(c.args().at(0), c.arg<std::string>(1)))));
    });
}

TEST_CASE("golden move_spelling_rules", "[golden]") {
    check_mutator("move_spelling_rules", [](const golden::Case&, Json& settings) -> Json {
        return text::move_spelling_rules(settings);
    });
}

TEST_CASE("golden add_term", "[golden]") {
    check_mutator("add_term", [](const golden::Case& c, Json& settings) -> Json {
        return plain(text::add_term(settings, c.arg<std::string>(1), heard_arg(c, 2, "heard"),
                                    flag_arg(c, 3, "starred")));
    });
}

TEST_CASE("golden update_term", "[golden]") {
    check_mutator("update_term", [](const golden::Case& c, Json& settings) -> Json {
        const std::string term_text =
            c.arg_count() > 2 ? c.arg<std::string>(2) : c.kwargs().at("term").get<std::string>();
        return plain(text::update_term(settings, c.arg<std::string>(1), term_text,
                                       heard_arg(c, 3, "heard")));
    });
}

TEST_CASE("golden set_starred", "[golden]") {
    check_mutator("set_starred", [](const golden::Case& c, Json& settings) -> Json {
        return plain(text::set_starred(settings, c.arg<std::string>(1), c.arg<bool>(2)));
    });
}

TEST_CASE("golden delete_term", "[golden]") {
    check_mutator("delete_term", [](const golden::Case& c, Json& settings) -> Json {
        return plain(text::delete_term(settings, c.arg<std::string>(1)));
    });
}

TEST_CASE("golden undo_learned", "[golden]") {
    check_mutator("undo_learned", [](const golden::Case& c, Json& settings) -> Json {
        return plain(text::undo_learned(settings, c.arg<std::string>(1)));
    });
}

TEST_CASE("golden clear_new", "[golden]") {
    check_mutator("clear_new", [](const golden::Case&, Json& settings) -> Json {
        return text::clear_new(settings);
    });
}

TEST_CASE("golden add_learned", "[golden]") {
    check_mutator("add_learned", [](const golden::Case& c, Json& settings) -> Json {
        return plain(
            text::add_learned(settings, c.arg<std::string>(1), flag_arg(c, 2, "protected")));
    });
}
