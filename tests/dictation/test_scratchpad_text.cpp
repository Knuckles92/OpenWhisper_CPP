// The Scratchpad's text rules (ui_qt/widgets/scratchpad.py _spaced, _utf16_slice, append_block,
// _show_word_count), replayed from tests/golden/ui_qt.widgets.scratchpad.json.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "dictation/scratchpad_text.h"
#include "golden/golden.h"

namespace {

using fv::dictation::append_block;
using fv::dictation::spaced;
using fv::dictation::utf16_slice;
using fv::dictation::word_count_text;

constexpr const char* kModule = "ui_qt.widgets.scratchpad";

} // namespace

TEST_CASE("a dictation is spaced into the words around it") {
    CHECK(spaced("Hello", "world", "") == " world");
    CHECK(spaced("Hello ", "world", "") == "world");
    CHECK(spaced("Hello", ".", "") == ".");
    CHECK(spaced("(", "aside", ")") == "aside");
    CHECK(spaced("", "word", "next") == "word ");
    CHECK(spaced("say", "word", "next") == " word ");
    CHECK(spaced("x", "word\n", "next") == " word\n");
    CHECK(spaced("x", "", "next").empty());
}

TEST_CASE("an emoji counts as one character on either side") {
    // The rule reads code points: an emoji before the text is not a no-space character, one
    // after it is not alphanumeric, and the window passes UTF-16 positions through utf16_slice.
    CHECK(spaced("\U0001f600", "ok", "") == " ok");
    CHECK(spaced("x", "ok", "\U0001f600") == " ok");
    const std::string text = "a\U0001f600b";
    CHECK(utf16_slice(text, 0, 1) == "a");
    CHECK(utf16_slice(text, 1, 3) == "\U0001f600");
    CHECK(utf16_slice(text, 3, 4) == "b");
    // Half of the pair is dropped, as decoding with errors="ignore" drops it.
    CHECK(utf16_slice(text, 0, 2) == "a");
    CHECK(utf16_slice(text, 2, 4) == "b");
}

TEST_CASE("a block is appended after a blank line") {
    CHECK(append_block("", "  Second.  ") == "Second.");
    CHECK(append_block("  \n", "Second.") == "Second.");
    CHECK(append_block("First.", "Second.") == "\n\nSecond.");
    CHECK(append_block("First.\n", "Second.") == "\nSecond.");
}

TEST_CASE("the word count reads as the status line") {
    CHECK(word_count_text("").empty());
    CHECK(word_count_text("  \n").empty());
    CHECK(word_count_text("one") == "1 word");
    CHECK(word_count_text(" one  two\tthree ") == "3 words");
    std::string many;
    for (int i = 0; i < 1234; ++i) {
        many += "x ";
    }
    CHECK(word_count_text(many) == "1,234 words");
}

TEST_CASE("golden scratchpad _spaced", "[golden]") {
    // One loop rather than golden::for_each's section per case: for_each reruns the test case
    // once per section, which over this table's 732 cases costs seconds.
    const fv::Json& cases = fv::golden::load(kModule).at("functions").at("_spaced");
    REQUIRE_FALSE(cases.empty());
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const fv::golden::Case c("_spaced", i, cases[i], 0.0);
        INFO(c.label());
        // Python's order is (text, before, after).
        CHECK(spaced(c.arg<std::string>(1), c.arg<std::string>(0), c.arg<std::string>(2)) ==
              c.result<std::string>());
    }
}

TEST_CASE("golden scratchpad _utf16_slice", "[golden]") {
    fv::golden::for_each(kModule, "_utf16_slice", [](const fv::golden::Case& c) {
        CHECK(utf16_slice(c.arg<std::string>(0), c.arg<std::int64_t>(1), c.arg<std::int64_t>(2)) ==
              c.result<std::string>());
    });
}

TEST_CASE("golden scratchpad append_block", "[golden]") {
    fv::golden::for_each(kModule, "append_block", [](const fv::golden::Case& c) {
        CHECK(append_block(c.arg<std::string>(0), c.arg<std::string>(1)) ==
              c.result<std::string>());
    });
}

TEST_CASE("golden scratchpad word count", "[golden]") {
    fv::golden::for_each(kModule, "word_count_text", [](const fv::golden::Case& c) {
        CHECK(word_count_text(c.arg<std::string>(0)) == c.result<std::string>());
    });
}
