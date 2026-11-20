#include <catch2/catch_test_macros.hpp>

#include "engine/transcript.h"

using fv::Segment;
using fv::Word;

TEST_CASE("segments end at sentence punctuation") {
    const std::vector<Word> words = {
        {"Hello", 0.0, 0.4}, {"there.", 0.5, 0.9}, {"How", 1.2, 1.4},
        {"are", 1.4, 1.5},   {"you?", 1.5, 1.9},   {"Fine", 2.5, 2.9},
    };
    const auto segments = fv::segments_from_words("ignored", words, 3.0);
    REQUIRE(segments == std::vector<Segment>{
                            {"Hello there.", 0.0, 0.9},
                            {"How are you?", 1.2, 1.9},
                            {"Fine", 2.5, 2.9},
                        });
}

TEST_CASE("a long run without punctuation is cut at the word limit") {
    std::vector<Word> words;
    for (std::size_t i = 0; i < fv::kSegmentMaxWords + 3; ++i) {
        words.push_back({"w", static_cast<double>(i), i + 0.5});
    }
    const auto segments = fv::segments_from_words("", words, 30.0);
    REQUIRE(segments.size() == 2);
    CHECK(segments[0].start == 0.0);
    CHECK(segments[0].end == fv::kSegmentMaxWords - 0.5);
    CHECK(segments[1].start == static_cast<double>(fv::kSegmentMaxWords));
    CHECK(segments[1].text == "w w w");
}

TEST_CASE("punctuation followed by whitespace still ends a segment") {
    const std::vector<Word> words = {{"Done! ", 0.0, 0.5}, {"Next", 0.6, 1.0}};
    const auto segments = fv::segments_from_words("", words, 1.0);
    REQUIRE(segments.size() == 2);
    CHECK(segments[0].text == "Done! ");
}

TEST_CASE("a sentence end is found after any whitespace Python strips") {
    // "Stop." + NO-BREAK SPACE, "Wait?" + IDEOGRAPHIC SPACE, "Odd!" + U+001C.
    const std::vector<Word> words = {{"Stop. ", 0.0, 0.5},    {"Wait?　", 0.6, 1.0},
                                     {"Odd!\x1c", 1.1, 1.5},  {"zero​.", 1.6, 2.0},
                                     {"width.​", 2.1, 2.5}, {"end", 2.6, 3.0}};
    const auto segments = fv::segments_from_words("", words, 3.0);
    REQUIRE(segments.size() == 5);
    CHECK(segments[3].text == "zero​.");
    // ZERO WIDTH SPACE is not whitespace to Python, so the period before it ends nothing.
    CHECK(segments[4].text == "width.​ end");
}

TEST_CASE("words are joined with one space each, empty ones included") {
    const std::vector<Word> words = {{"", 0.0, 0.1}, {"x", 0.2, 0.3}, {"", 0.4, 0.4}};
    const auto segments = fv::segments_from_words("", words, 1.0);
    REQUIRE(segments.size() == 1);
    CHECK(segments[0].text == " x ");
}

TEST_CASE("text without word timings is one segment over the whole audio") {
    const auto segments = fv::segments_from_words("no timings here", {}, 4.5);
    REQUIRE(segments == std::vector<Segment>{{"no timings here", 0.0, 4.5}});
}

TEST_CASE("blank text without word timings gives no segments") {
    CHECK(fv::segments_from_words("", {}, 4.5).empty());
    CHECK(fv::segments_from_words("  \n", {}, 4.5).empty());
    CHECK(fv::segments_from_words("　 \x1f", {}, 4.5).empty());
}
