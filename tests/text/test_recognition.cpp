// tests/test_personalize_s4_recognition.py TestHelpers (language codes, OpenAI keywords and the
// vocabulary prompt; the backend tests are M4.9's and M9's); plus the
// services.recognition_context golden vectors.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "base/json.h"
#include "golden/golden.h"
#include "text/recognition.h"

using Catch::Matchers::EndsWith;
using fv::Json;
namespace text = fv::text;
namespace golden = fv::golden;

TEST_CASE("TestHelpers::test_language_code", "[test_personalize_s4_recognition]") {
    // from tests/test_personalize_s4_recognition.py::TestHelpers::test_language_code
    const std::vector<std::pair<std::optional<std::string_view>, std::string>> rows{
        {"", ""},     {std::nullopt, ""}, {"auto", ""},    {"AUTO", ""},
        {"en", "en"}, {"pt-BR", "pt"},    {"zh_CN", "zh"},
    };
    for (const auto& [language, code] : rows) {
        INFO(language.value_or("None"));
        CHECK(text::language_code(language) == code);
    }
}

TEST_CASE("TestHelpers::test_keywords_and_prompt", "[test_personalize_s4_recognition]") {
    // from tests/test_personalize_s4_recognition.py::TestHelpers::test_keywords_and_prompt
    std::vector<std::string> sixty;
    for (int i = 0; i < 60; ++i) {
        sixty.push_back("w" + std::to_string(i));
    }
    CHECK(text::keywords(sixty) == std::vector<std::string>(sixty.begin(), sixty.begin() + 50));
    CHECK(text::keywords({"  spaced   out ", std::string(65, 'x')}) ==
          std::vector<std::string>{"spaced out"});
    CHECK(text::vocabulary_prompt({"Ksenia", "Kubernetes"}) == "Ksenia, Kubernetes.");
    CHECK(text::vocabulary_prompt({}).empty());
    std::vector<std::string> many;
    for (int i = 0; i < 200; ++i) {
        char word[16];
        std::snprintf(word, sizeof word, "Word%03d", i);
        many.emplace_back(word);
    }
    const std::string long_prompt = text::vocabulary_prompt(many);
    CHECK(long_prompt.size() <= text::OPENAI_PROMPT_MAX_CHARS);
    CHECK_THAT(long_prompt, EndsWith("."));
}

TEST_CASE("Parakeet takes phrases only after recognition") {
    CHECK(text::engine_support("parakeet") == text::SUPPORT_AFTER);
    CHECK(text::engine_support("api") == text::SUPPORT_MODEL);
    CHECK(text::engine_support("remote").empty());
}

// --- golden vectors -------------------------------------------------------------------------

TEST_CASE("golden engine_support", "[golden]") {
    golden::for_each("services.recognition_context", "engine_support", [](const golden::Case& c) {
        CHECK(text::engine_support(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden language_code", "[golden]") {
    golden::for_each("services.recognition_context", "language_code", [](const golden::Case& c) {
        const auto language = c.arg<std::optional<std::string>>(0);
        CHECK(text::language_code(language ? std::optional<std::string_view>(*language)
                                           : std::nullopt) == c.result<std::string>());
    });
}

TEST_CASE("golden keywords", "[golden]") {
    golden::for_each("services.recognition_context", "keywords", [](const golden::Case& c) {
        CHECK(text::keywords(c.arg<std::vector<std::string>>(0)) ==
              c.result<std::vector<std::string>>());
    });
}

TEST_CASE("golden vocabulary_prompt", "[golden]") {
    golden::for_each(
        "services.recognition_context", "vocabulary_prompt", [](const golden::Case& c) {
            const auto phrases = c.arg<std::vector<std::string>>(0);
            const std::string got = c.arg_count() > 1
                                        ? text::vocabulary_prompt(phrases, c.arg<std::size_t>(1))
                                        : text::vocabulary_prompt(phrases);
            CHECK(got == c.result<std::string>());
        });
}
