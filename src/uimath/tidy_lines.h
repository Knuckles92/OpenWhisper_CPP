#pragma once

#include <array>
#include <vector>

namespace fv {

// Cleaning up and Rewriting: three lines of scribbled word strokes a caret combs straight.
// One pass of the caret, then the lines hold, loosen, and it goes again.
inline constexpr double kTidyPeriodS = 2.1;
inline constexpr double kTidyCombS = 1.35;
inline constexpr double kTidyLoosenAtS = 1.75;
inline constexpr double kTidyLoosenS = 0.35;
// How long the lines take to appear.
inline constexpr double kTidyAppearS = 0.35;
inline constexpr double kTidyHalfSpan = 66.0;
inline constexpr double kTidyLinePitch = 9.0;
inline constexpr double kTidyWordGap = 5.0;
inline constexpr double kTidyStroke = 3.0;

// One word of a line, as OpenWhisper's _scribble lays it out.
struct ScribbleWord {
    double length;
    // Offsets while the word is still loose.
    double dy;
    double dx;
    // The wobble phase, in radians.
    double phase;
};
using ScribbleLine = std::vector<ScribbleWord>;

// The words of the three lines (top to bottom), fitted to 2 * kTidyHalfSpan. Python lays them
// out with random.Random(7), whose stream std::mt19937 cannot reproduce, so the table is read
// from data/overlay_tables.json (tools/gen/overlay_tables.py). Parsed once; throws
// std::runtime_error if the embedded table is malformed.
const std::array<ScribbleLine, 3>& scribble_lines();

} // namespace fv
