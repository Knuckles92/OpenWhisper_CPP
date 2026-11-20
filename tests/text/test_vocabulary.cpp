// tests/test_personalize_fix2_g4_fold_speed.py: term replacement folds case for the whole text
// at once, matching the per-character fold; plus the services.vocabulary golden vectors.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "base/json.h"
#include "golden/golden.h"
#include "pyport/text.h"
#include "pyport/unicode.h"
#include "text/dictionary.h"
#include "text/vocabulary.h"

using fv::Json;
using fv::text::fold_with_origin;
using fv::text::FoldedText;
using fv::text::replace_terms;
namespace golden = fv::golden;

namespace {

using Rules = std::vector<std::pair<std::string, std::string>>;

constexpr char32_t kDot = 0x0307; // COMBINING DOT ABOVE
const std::string kDotUtf8 = "\xCC\x87";

std::string utf8(char32_t cp) {
    std::string out;
    fv::py::append_utf8(out, cp);
    return out;
}

// Folds one character at a time: plainly right, too slow for Meeting Mode.
FoldedText reference_fold_with_origin(const std::string& text) {
    const auto fold = [](const std::string& piece) {
        std::string folded = fv::py::casefold(piece);
        const std::string dotted = "i" + kDotUtf8;
        for (std::size_t pos = folded.find(dotted); pos != std::string::npos;
             pos = folded.find(dotted, pos + 1)) {
            folded.replace(pos, dotted.size(), "i");
        }
        return fv::py::to_code_points(folded);
    };
    FoldedText out;
    std::u32string folded;
    char32_t previous = 0;
    const std::u32string chars = fv::py::to_code_points(text);
    for (std::size_t index = 0; index < chars.size(); ++index) {
        const char32_t ch = chars[index];
        std::u32string piece;
        if (!(ch == kDot && previous == U'i')) {
            piece = fold(utf8(ch));
        }
        folded += piece;
        out.origin.insert(out.origin.end(), piece.size(), index);
        if (!piece.empty()) {
            previous = piece.back();
        }
    }
    out.folded = fv::py::to_utf8(folded);
    return out;
}

void check_against_reference(const std::string& text) {
    const FoldedText got = fold_with_origin(text);
    const FoldedText expected = reference_fold_with_origin(text);
    INFO(fv::py::repr(text));
    CHECK(got.folded == expected.folded);
    CHECK(got.origin == expected.origin);
}

Rules rules_from(const Json& raw) {
    Rules rules;
    if (raw.is_object()) {
        for (const auto& [term, replacement] : raw.items()) {
            rules.emplace_back(term, replacement.get<std::string>());
        }
    } else {
        for (const Json& pair : raw) {
            rules.emplace_back(pair.at(0).get<std::string>(), pair.at(1).get<std::string>());
        }
    }
    return rules;
}

std::vector<fv::text::DictionaryTerm> izmir_terms(const std::string& heard) {
    Json entry = Json::object();
    entry["id"] = "t";
    entry["term"] = "Izmir HQ";
    entry["starred"] = false;
    entry["heard"] = Json::array({heard});
    return fv::text::parse_terms(Json::array({entry}));
}

} // namespace

// from
// tests/test_personalize_fix2_g4_fold_speed.py::test_no_character_folds_to_nothing_and_only_the_dotted_capital_i_folds_to_a_dot
TEST_CASE("test_no_character_folds_to_nothing_and_only_the_dotted_capital_i_folds_to_a_dot",
          "[test_personalize_fix2_g4_fold_speed]") {
    // The whole-text fold relies on both facts about Python's Unicode tables.
    std::vector<char32_t> empty;
    std::vector<char32_t> dotted;
    for (char32_t code = 0; code <= 0x10FFFF; ++code) {
        const std::string folded = fv::py::casefold(utf8(code));
        if (folded.empty()) {
            empty.push_back(code);
        }
        if (code != kDot && folded.find(kDotUtf8) != std::string::npos) {
            dotted.push_back(code);
        }
    }
    CHECK(empty.empty());
    CHECK(dotted == std::vector<char32_t>{0x130});
}

// from tests/test_personalize_fix2_g4_fold_speed.py::test_the_fold_matches_the_per_character_fold
TEST_CASE("test_the_fold_matches_the_per_character_fold", "[test_personalize_fix2_g4_fold_speed]") {
    std::string ascii;
    for (int code = 0; code < 128; ++code) {
        ascii.push_back(static_cast<char>(code));
    }
    const std::string text = GENERATE_COPY(values<std::string>({
        "",
        "plain ascii, with digits 123!",
        ascii,
        "café — we’ll ship",
        "die Straße hier",
        "İİ İzmir",
        "i" + kDotUtf8 + "zmir",
        "I" + kDotUtf8 + kDotUtf8 + "x",
        "ﬁ" + kDotUtf8 + " a" + kDotUtf8 + kDotUtf8,
        kDotUtf8 + "i",
        "ΑΣ ΣΑΣ σς",
    }));
    check_against_reference(text);
}

// from
// tests/test_personalize_fix2_g4_fold_speed.py::test_the_fold_matches_the_per_character_fold_on_random_text
TEST_CASE("test_the_fold_matches_the_per_character_fold_on_random_text",
          "[test_personalize_fix2_g4_fold_speed]") {
    // Python draws from random.Random(7); this draws the same alphabet from its own seeded
    // generator.
    std::vector<std::string> alphabet;
    for (const char32_t cp : fv::py::to_code_points("abcxyz iIİıßẞﬁﬃΣσςÉéŉǰ’—.,KkK")) {
        alphabet.push_back(utf8(cp));
    }
    alphabet.insert(alphabet.end(), {kDotUtf8, kDotUtf8, "İ"});
    std::mt19937 rng(7);
    std::uniform_int_distribution<std::size_t> length(0, 14);
    std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
    std::size_t mismatches = 0;
    for (int round = 0; round < 20000; ++round) {
        std::string text;
        for (std::size_t n = length(rng); n > 0; --n) {
            text += alphabet[pick(rng)];
        }
        const FoldedText got = fold_with_origin(text);
        const FoldedText expected = reference_fold_with_origin(text);
        if (got.folded != expected.folded || got.origin != expected.origin) {
            ++mismatches;
            UNSCOPED_INFO(fv::py::repr(text));
        }
    }
    CHECK(mismatches == 0);
}

// from
// tests/test_personalize_fix2_g4_fold_speed.py::test_long_text_costs_no_more_python_calls_than_short_text
TEST_CASE("test_long_text_costs_no_more_python_calls_than_short_text",
          "[test_personalize_fix2_g4_fold_speed]") {
    // Meeting Mode corrects every segment of every transcript read. Python counts interpreter
    // calls; here the cost must be linear: 1 MiB of text takes less than 20 times as long as
    // 64 KiB (16 times the text).
    const std::string filler = GENERATE(values<std::string>({
        "the quick brown fox ",
        "we’ll ship the café — ",
        "die Straße ist groß ",
        "İyi günler İZMİR ",
    }));
    const Rules rules = {{"kubernetes", "Kubernetes"}, {"jira", "Jira"}};
    const auto text_of = [&](std::size_t bytes) {
        std::string text;
        while (text.size() < bytes) {
            text += filler;
        }
        return text + "ask jira about kubernetes.";
    };
    const std::string small = text_of(64 * 1024);
    const std::string large = text_of(1024 * 1024);
    const auto time_of = [&](const std::string& text) {
        const auto start = std::chrono::steady_clock::now();
        const std::string result = replace_terms(text, rules);
        const auto took = std::chrono::steady_clock::now() - start;
        CHECK(result.ends_with("ask Jira about Kubernetes."));
        return std::chrono::duration<double>(took).count();
    };
    // A linear cost puts the ratio near 16, and wall time on a shared machine is noisy: the two
    // sizes are timed in turn, so both see the same load, each keeps its best of seven runs, and
    // a ratio over the bound is measured again (at most five times) before it counts as a
    // failure.
    double small_time = 0;
    double large_time = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        small_time = std::numeric_limits<double>::max();
        large_time = std::numeric_limits<double>::max();
        for (int run = 0; run < 7; ++run) {
            small_time = std::min(small_time, time_of(small));
            large_time = std::min(large_time, time_of(large));
        }
        if (large_time < 20 * small_time) {
            break;
        }
    }
    INFO("64 KiB: " << small_time << " s, 1 MiB: " << large_time << " s");
    CHECK(large_time < 20 * small_time);
}

// Dictionary and term replacements match letters whose case forms differ in length.

// from tests/test_personalize_fix2_g4_fold_speed.py::test_a_dotted_capital_i_matches_its_variant
TEST_CASE("test_a_dotted_capital_i_matches_its_variant", "[test_personalize_fix2_g4_fold_speed]") {
    const std::string heard = GENERATE(values<std::string>({"izmir", "İzmir", "IZMIR"}));
    const std::string spoken =
        GENERATE(values<std::string>({"İzmir office", "izmir office", "IZMİR office"}));
    CHECK(fv::text::apply_replacements(spoken, izmir_terms(heard)) == "Izmir HQ office");
}

// from
// tests/test_personalize_fix2_g4_fold_speed.py::test_the_dictation_pipeline_replaces_a_dotted_capital_i
TEST_CASE("test_the_dictation_pipeline_replaces_a_dotted_capital_i",
          "[test_personalize_fix2_g4_fold_speed]") {
    // prepare_text applies the dictionary through apply_replacements, which is what this pins;
    // the preparer itself is fv_dictation's.
    Json entry = Json::object();
    entry["id"] = "t";
    entry["term"] = "Istanbul Ofis";
    entry["heard"] = Json::array({"İstanbul"});
    Json settings = Json::object();
    settings["dictation_dictionary"] = Json::array({entry});
    CHECK(fv::text::apply_replacements("İstanbul ofis", fv::text::load_dictionary(settings)) ==
          "Istanbul Ofis ofis");
}

// from tests/test_personalize_fix2_g4_fold_speed.py::test_replace_terms_folds_case_like_unicode
TEST_CASE("test_replace_terms_folds_case_like_unicode", "[test_personalize_fix2_g4_fold_speed]") {
    struct Row {
        std::string text;
        Rules rules;
        std::string replaced;
    };
    const auto row = GENERATE_COPY(values<Row>({
        {"İzmir office", {{"izmir", "Izmir HQ"}}, "Izmir HQ office"},
        {"ſoft launch", {{"soft", "Soft"}}, "Soft launch"},
        {"die Straße hier", {{"strasse", "Strasse"}}, "die Strasse hier"},
        // Text after a letter that folds to two keeps its place.
        {"Straße und koln.", {{"koln", "Köln"}}, "Straße und Köln."},
        {"İİ İzmir", {{"izmir", "X"}}, "İİ X"},
        // A lowercased dotted capital (i + combining dot) is still the word.
        {"i" + kDotUtf8 + "zmir", {{"izmir", "Izmir"}}, "Izmir"},
        {"Izmirli", {{"izmir", "X"}}, "Izmirli"},
        {"x", {{"", "never"}}, "x"},
        {"we’ll ship kubernetes — café",
         {{"kubernetes", "Kubernetes"}},
         "we’ll ship Kubernetes — café"},
        {"an i" + kDotUtf8 + kDotUtf8 + "zmir trip", {{"izmir", "Izmir"}}, "an Izmir trip"},
        {"x ﬁ" + kDotUtf8 + "x", {{"fix", "FIX"}}, "x FIX"},
        {"İZMİR", {{"i" + kDotUtf8 + "zmir", "Izmir"}}, "Izmir"},
    }));
    CHECK(replace_terms(row.text, row.rules) == row.replaced);
}

// from
// tests/test_personalize_fix2_g4_fold_speed.py::test_meeting_rules_with_lowered_keys_still_match
TEST_CASE("test_meeting_rules_with_lowered_keys_still_match",
          "[test_personalize_fix2_g4_fold_speed]") {
    // Meeting corrections (fv_meeting_state) key their rules by the selected text lowered, so
    // "İzmir" becomes "i" + U+0307 + "zmir"; correct_text is replace_terms over those rules.
    const Rules rules = {{fv::py::lower("İzmir"), "Izmir"}};
    CHECK(replace_terms("we met in İzmir", rules) == "we met in Izmir");
}

TEST_CASE("a full dictionary's variants replace in one pass", "[vocabulary]") {
    // 500 words with 5 variants each: more alternatives than one PCRE2 pattern holds.
    Rules rules;
    for (int word = 0; word < 500; ++word) {
        for (int variant = 0; variant < 5; ++variant) {
            rules.emplace_back("variant" + std::to_string(word) + "x" + std::to_string(variant) +
                                   std::string(40, 'q'),
                               "W" + std::to_string(word));
        }
    }
    const std::string text =
        "say VARIANT499X4" + std::string(40, 'Q') + " and variant0x0" + std::string(40, 'q') + ".";
    CHECK(replace_terms(text, rules) == "say W499 and W0.");
}

TEST_CASE("golden _fold_with_origin", "[golden]") {
    golden::for_each("services.vocabulary", "_fold_with_origin", [](const golden::Case& c) {
        const FoldedText got = fold_with_origin(c.arg<std::string>(0));
        const Json result = c.result<Json>();
        CHECK(got.folded == result.at(0).get<std::string>());
        CHECK(got.origin == result.at(1).get<std::vector<std::size_t>>());
    });
}

TEST_CASE("golden replace_terms", "[golden]") {
    golden::for_each("services.vocabulary", "replace_terms", [](const golden::Case& c) {
        CHECK(replace_terms(c.arg<std::string>(0), rules_from(c.args().at(1))) ==
              c.result<std::string>());
    });
}
