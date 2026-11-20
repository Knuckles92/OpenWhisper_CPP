// tests/test_personalize_s4_learning.py (the correction diff) and
// tests/test_dictionary_cleanup_revert.py (reverted cleanup, protected words): the pure half of
// learning from corrections. The scheduler, the re-read and the sightings are fv_dictation's.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json.h"
#include "golden/golden.h"
#include "text/dictionary.h"
#include "text/dictionary_learning.h"
#include "text_test_support.h"

using fv::Json;
using fv::text_test::kDictionary;
namespace text = fv::text;
namespace golden = fv::golden;

namespace {

const std::string kPasted = "Let's meet with Sonia about the launch.";

// Python's learned_correction: the user's word of the correction.
std::optional<std::string> learned_word(std::string_view pasted, std::string_view field) {
    const auto found = text::learned_correction(pasted, field);
    return found ? std::optional<std::string>(found->second) : std::nullopt;
}

} // namespace

// ---- test_personalize_s4_learning.py: TestCorrectionDiff ---------------------------------------

// from tests/test_personalize_s4_learning.py::TestCorrectionDiff::test_single_word_substitution
TEST_CASE("TestCorrectionDiff::test_single_word_substitution", "[test_personalize_s4_learning]") {
    const auto [field, expected] = GENERATE(values<std::pair<std::string, std::string>>({
        {"Dear team, Let's meet with Ksenia about the launch.", "Ksenia"},
        {"let's meet with KSENIA about the launch. See you then", "KSENIA"},
        {"Let's meet with Ksenia about the launch", "Ksenia"},
        {"Let's meet with Sonia about the Kickoff.", "Kickoff"},
    }));
    CHECK(learned_word(kPasted, field) == expected);
}

// from tests/test_personalize_s4_learning.py::TestCorrectionDiff::test_everything_else_is_ignored
TEST_CASE("TestCorrectionDiff::test_everything_else_is_ignored", "[test_personalize_s4_learning]") {
    const std::string field = GENERATE(values<std::string>({
        "Let's meet with Sonia about the launch.",          // unchanged
        "Let's meet with sonia about the launch.",          // case only
        "Let's meet with Ksenia Petrova about the launch.", // one word became two
        "Let's meet with Ksenia about the big launch.",     // two edits
        "Let's meet with Ksenia and the launch.",           // two edits
        "Let's meet with Monday about the launch.",         // a common word
        "Let's meet with Al about the launch.",             // too short
        "Let's meet with S0n1a_ about the launch.",         // not a word
        "Let's meet with 2024 about the launch.",           // digits only
        "Completely different text in the field.",
        "",
    }));
    CHECK_FALSE(text::learned_correction(kPasted, field));
}

// from
// tests/test_personalize_s4_learning.py::TestCorrectionDiff::test_needs_two_unchanged_words_to_anchor
TEST_CASE("TestCorrectionDiff::test_needs_two_unchanged_words_to_anchor",
          "[test_personalize_s4_learning]") {
    CHECK_FALSE(text::learned_correction("Call Sonia", "Call Ksenia"));
    CHECK(learned_word("Call Sonia now", "Call Ksenia now") == "Ksenia");
    const auto pair = text::learned_correction("Call Sonia now", "Call Ksenia now");
    REQUIRE(pair);
    CHECK(pair->first == "Sonia");
}

// from
// tests/test_personalize_s4_learning.py::TestCorrectionDiff::test_apostrophe_or_hyphen_only_is_not_a_correction
TEST_CASE("TestCorrectionDiff::test_apostrophe_or_hyphen_only_is_not_a_correction",
          "[test_personalize_s4_learning]") {
    CHECK_FALSE(text::learned_correction("we dont ship today", "we don't ship today"));
    CHECK_FALSE(text::learned_correction("the eshop is open", "the e-shop is open"));
}

// ---- test_dictionary_cleanup_revert.py --------------------------------------------------------

// from tests/test_dictionary_cleanup_revert.py::test_reverted_cleanup
TEST_CASE("test_reverted_cleanup", "[test_dictionary_cleanup_revert]") {
    struct Row {
        std::optional<std::string> before_cleanup;
        std::string replaced;
        std::string restored;
        bool expected;
    };
    const std::string spoken = "lets meet with Ksenia about the launch";
    const auto row = GENERATE_COPY(values<Row>({
        {spoken, "Sonia", "Ksenia", true},
        {"lets meet with ksenia about the launch", "Sonia", "KSENIA", true},
        {spoken, "Ksenia", "Sonia", false}, // the user changed a spoken word
        {"lets meet with Sonia about the launch", "Sonia", "Ksenia", false}, // cleanup kept it
        {std::nullopt, "Sonia", "Ksenia", false}, // cleanup changed nothing
        {"", "Sonia", "Ksenia", false},
    }));
    const std::optional<std::string_view> before =
        row.before_cleanup ? std::optional<std::string_view>(*row.before_cleanup) : std::nullopt;
    CHECK(text::reverted_cleanup(before, row.replaced, row.restored) == row.expected);
}

// from
// tests/test_dictionary_cleanup_revert.py::test_protected_words_round_trip_and_reach_the_cleanup_prompt
TEST_CASE("test_protected_words_round_trip_and_reach_the_cleanup_prompt",
          "[test_dictionary_cleanup_revert]") {
    Json settings = Json::object();
    text::add_term(settings, "Kubernetes");
    const auto kept = text::add_learned(settings, "Ksenia", true);
    REQUIRE(kept);
    CHECK(kept->is_protected);
    CHECK(settings.at(kDictionary).at(0).at("protected") == true);
    CHECK_FALSE(settings.at(kDictionary).at(1).contains("protected"));

    const auto terms = text::load_dictionary(settings);
    REQUIRE(terms.size() == 2);
    CHECK((terms[0].term == "Ksenia" && terms[0].is_protected));
    CHECK((terms[1].term == "Kubernetes" && !terms[1].is_protected));
    const std::string block = text::prompt_block(terms);
    CHECK_THAT(block, Catch::Matchers::ContainsSubstring("spell it exactly: Ksenia, Kubernetes."));
    CHECK_THAT(block, Catch::Matchers::EndsWith("never replace or respell them: Ksenia."));
    CHECK(text::find_term(settings, "ksenia") == kept);
}

// from tests/test_dictionary_cleanup_revert.py::test_no_protected_words_leaves_the_prompt_unchanged
TEST_CASE("test_no_protected_words_leaves_the_prompt_unchanged",
          "[test_dictionary_cleanup_revert]") {
    Json settings = Json::object();
    text::add_term(settings, "Kubernetes");
    CHECK_THAT(text::prompt_block(text::load_dictionary(settings)),
               !Catch::Matchers::ContainsSubstring("never replace"));
}

TEST_CASE("the learning stoplist is the generated COMMON_WORDS", "[dictionary_learning]") {
    CHECK(text::is_common_word("monday"));
    CHECK(text::is_common_word("thousand"));
    CHECK_FALSE(text::is_common_word("Monday")); // the list is casefolded
    CHECK_FALSE(text::is_common_word("ksenia"));
    CHECK(text::correction_tokens(" «Hi», (there)… _x_ ") ==
          std::vector<std::string>{"Hi", "there", "_x_"});
}

// ---- golden vectors ---------------------------------------------------------------------------

TEST_CASE("golden _correction", "[golden]") {
    golden::for_each("services.dictionary", "_correction", [](const golden::Case& c) {
        const auto found = text::learned_correction(c.arg<std::string>(0), c.arg<std::string>(1));
        const Json got = found ? Json::array({found->first, found->second}) : Json(nullptr);
        CHECK(golden::equal(c.result<Json>(), got));
    });
}

TEST_CASE("golden learned_correction", "[golden]") {
    golden::for_each("services.dictionary", "learned_correction", [](const golden::Case& c) {
        CHECK(learned_word(c.arg<std::string>(0), c.arg<std::string>(1)) ==
              c.result<std::optional<std::string>>());
    });
}

TEST_CASE("golden reverted_cleanup", "[golden]") {
    golden::for_each("services.dictionary", "reverted_cleanup", [](const golden::Case& c) {
        const auto before = c.arg<std::optional<std::string>>(0);
        const std::optional<std::string_view> view =
            before ? std::optional<std::string_view>(*before) : std::nullopt;
        CHECK(text::reverted_cleanup(view, c.arg<std::string>(1), c.arg<std::string>(2)) ==
              c.result<bool>());
    });
}
