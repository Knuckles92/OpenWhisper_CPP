// Ported from tests/test_fuzzy_match.py, plus the golden vectors of ui_qt.utils.fuzzy_match.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include "golden/golden.h"
#include "uimath/fuzzy_match.h"

using fv::fuzzy_match;
using fv::fuzzy_score;

TEST_CASE("fuzzy_score replays the golden vectors", "[golden][fuzzy_match]") {
    fv::golden::for_each("ui_qt.utils.fuzzy_match", "fuzzy_score", [](const fv::golden::Case& c) {
        CHECK(fuzzy_score(c.arg<std::string>(0), c.arg<std::string>(1)) ==
              c.result<std::optional<int>>());
    });
}

TEST_CASE("fuzzy_match replays the golden vectors", "[golden][fuzzy_match]") {
    fv::golden::for_each("ui_qt.utils.fuzzy_match", "fuzzy_match", [](const fv::golden::Case& c) {
        CHECK(fuzzy_match(c.arg<std::string>(0), c.arg<std::string>(1)) == c.result<bool>());
    });
}

// from tests/test_fuzzy_match.py::test_empty_query_matches_everything
TEST_CASE("empty query matches everything", "[test_fuzzy_match]") {
    CHECK(fuzzy_match("", "google/gemini-3.7-flash"));
    CHECK(fuzzy_score("", "google/gemini-3.7-flash") == 0);
    CHECK(fuzzy_match("   ", "anything"));
    CHECK(fuzzy_score("   ", "anything") == 0);
}

// from tests/test_fuzzy_match.py::test_empty_target_does_not_match_non_empty_query
TEST_CASE("empty target does not match non empty query", "[test_fuzzy_match]") {
    CHECK_FALSE(fuzzy_match("gemini", ""));
    CHECK(fuzzy_score("gemini", "") == std::nullopt);
}

// from tests/test_fuzzy_match.py::test_exact_match
TEST_CASE("exact match", "[test_fuzzy_match]") {
    const std::optional<int> score = fuzzy_score("openai/gpt-4o", "openai/gpt-4o");
    REQUIRE(score.has_value());
    CHECK(*score >= 10000);
}

// from tests/test_fuzzy_match.py::test_case_insensitivity
TEST_CASE("case insensitivity", "[test_fuzzy_match]") {
    CHECK(fuzzy_match("GEMINI", "google/gemini-3.7-flash"));
    CHECK(fuzzy_match("gemini", "GOOGLE/GEMINI-3.7-FLASH"));
}

// from tests/test_fuzzy_match.py::test_space_vs_hyphen_delimiter_matching
TEST_CASE("space vs hyphen delimiter matching", "[test_fuzzy_match]") {
    CHECK(fuzzy_match("gemini 3.7", "google/gemini-3.7-flash"));
    CHECK(fuzzy_match("gemini 3.7", "gemini-3.7"));
    CHECK(fuzzy_match("gpt 4o", "openai/gpt-4o"));
    CHECK(fuzzy_match("gpt 4o mini", "openai/gpt-4o-mini"));
    CHECK(fuzzy_match("claude 3.5 sonnet", "anthropic/claude-3.5-sonnet"));
}

// from tests/test_fuzzy_match.py::test_multi_token_out_of_order_matching
TEST_CASE("multi token out of order matching", "[test_fuzzy_match]") {
    CHECK(fuzzy_match("claude sonnet", "anthropic/claude-3.5-sonnet"));
    CHECK(fuzzy_match("sonnet claude", "anthropic/claude-3.5-sonnet"));
    CHECK(fuzzy_match("llama 70b", "meta-llama/llama-3.1-70b-instruct"));
    CHECK(fuzzy_match("70b llama", "meta-llama/llama-3.1-70b-instruct"));
}

// from tests/test_fuzzy_match.py::test_stripped_delimiter_matching
TEST_CASE("stripped delimiter matching", "[test_fuzzy_match]") {
    CHECK(fuzzy_match("gpt4o", "openai/gpt-4o"));
    CHECK(fuzzy_match("gemini3.7", "google/gemini-3.7-flash"));
    CHECK(fuzzy_match("o3mini", "openai/o3-mini"));
}

// from tests/test_fuzzy_match.py::test_subsequence_fuzzy_matching
TEST_CASE("subsequence fuzzy matching", "[test_fuzzy_match]") {
    CHECK(fuzzy_match("g3.7", "google/gemini-3.7-flash"));
    CHECK(fuzzy_match("c35s", "anthropic/claude-3.5-sonnet"));
    CHECK(fuzzy_match("dskr1", "deepseek/deepseek-r1"));
}

// from tests/test_fuzzy_match.py::test_negative_matches
TEST_CASE("negative matches", "[test_fuzzy_match]") {
    CHECK_FALSE(fuzzy_match("gemini 3.7", "google/gemini-2.5-flash"));
    CHECK_FALSE(fuzzy_match("gemini 3.7", "openai/gpt-4o"));
    CHECK_FALSE(fuzzy_match("nonexistent", "google/gemini-3.7-flash"));
    CHECK_FALSE(fuzzy_match("xyz123", "anthropic/claude-3.5-sonnet"));
}

// from tests/test_fuzzy_match.py::test_regex_special_characters_do_not_error
TEST_CASE("regex special characters do not error", "[test_fuzzy_match]") {
    CHECK(fuzzy_match("gemini (3.7)", "google/gemini-3.7-flash"));
    CHECK(fuzzy_match("gpt+4o", "openai/gpt-4o"));
    CHECK(fuzzy_match("claude-3.5*", "anthropic/claude-3.5-sonnet"));
    CHECK(fuzzy_match("[test]", "test/model"));
    CHECK_FALSE(fuzzy_match("???", "google/gemini-3.7-flash"));
}

// from tests/test_fuzzy_match.py::test_score_ranking_relevance
TEST_CASE("score ranking relevance", "[test_fuzzy_match]") {
    const std::optional<int> exact = fuzzy_score("gemini-3.7", "gemini-3.7");
    const std::optional<int> substring = fuzzy_score("gemini-3.7", "google/gemini-3.7-flash");
    const std::optional<int> normalized = fuzzy_score("gemini 3.7", "google/gemini-3.7-flash");
    const std::optional<int> subsequence = fuzzy_score("g37", "google/gemini-3.7-flash");
    REQUIRE(exact.has_value());
    REQUIRE(substring.has_value());
    REQUIRE(normalized.has_value());
    REQUIRE(subsequence.has_value());
    CHECK(*exact > *substring);
    CHECK(*substring >= *normalized);
    CHECK(*normalized > *subsequence);
}
