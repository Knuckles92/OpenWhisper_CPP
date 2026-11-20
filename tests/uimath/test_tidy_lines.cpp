// The scribble table of ui_qt.overlays.tidy_lines. Python draws it with random.Random(7); the C++
// side reads the dumped table, so the tests check its shape and the layout rule it came from.
#include <catch2/catch_test_macros.hpp>

#include <numbers>

#include "uimath/tidy_lines.h"

TEST_CASE("the scribble table has three lines of words fitted to the span", "[tidy_lines]") {
    const auto& lines = fv::scribble_lines();
    REQUIRE(lines.size() == 3);
    for (const auto& line : lines) {
        REQUIRE(!line.empty());
        double x = 0.0;
        for (const auto& word : line) {
            CHECK(word.length >= 8.0);
            CHECK(word.length <= 24.0);
            CHECK(word.dy >= -3.5);
            CHECK(word.dy <= 3.5);
            CHECK(word.dx >= -4.0);
            CHECK(word.dx <= 4.0);
            CHECK(word.phase >= 0.0);
            CHECK(word.phase <= 2.0 * std::numbers::pi);
            // The generator stops at the first word that would pass the span.
            CHECK(x + word.length <= 2 * fv::kTidyHalfSpan);
            x += word.length + fv::kTidyWordGap;
        }
        // The word drawn next (24 long at most) did not fit, or the line would hold it.
        CHECK(x + 24.0 > 2 * fv::kTidyHalfSpan);
    }
}

TEST_CASE("the scribble table is read once", "[tidy_lines]") {
    CHECK(&fv::scribble_lines() == &fv::scribble_lines());
}

TEST_CASE("the tidy timings are OpenWhisper's", "[tidy_lines]") {
    CHECK(fv::kTidyPeriodS == 2.1);
    CHECK(fv::kTidyCombS == 1.35);
    CHECK(fv::kTidyLoosenAtS == 1.75);
    CHECK(fv::kTidyLoosenS == 0.35);
    CHECK(fv::kTidyAppearS == 0.35);
    CHECK(fv::kTidyHalfSpan == 66.0);
    CHECK(fv::kTidyLinePitch == 9.0);
    CHECK(fv::kTidyWordGap == 5.0);
    CHECK(fv::kTidyStroke == 3.0);
}

TEST_CASE("the scribble table is OpenWhisper's random.Random(7) layout", "[tidy_lines]") {
    const auto& lines = fv::scribble_lines();
    CHECK(lines[0].size() == 6);
    CHECK(lines[1].size() == 7);
    CHECK(lines[2].size() == 6);
    // The first and last words as tools/gen/overlay_tables.py dumped them.
    CHECK(lines[0].front().length == 13.181324237330598);
    CHECK(lines[0].front().phase == 0.45513061209615324);
    CHECK(lines[2].back().dy == -1.5078312753409553);
}
