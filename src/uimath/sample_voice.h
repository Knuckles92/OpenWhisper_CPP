#pragma once

#include <array>
#include <string_view>
#include <vector>

namespace fv {

// The Settings preview's sample voice (recording_look_preview): two phrases of words at the
// levels a desk microphone reports, then the look's own finish, looping.
inline constexpr double kLoopS = 9.4;

struct SampleStage {
    std::string_view name;
    // When the stage starts, in seconds into the loop.
    double start;
};

// Each finish stage gets long enough to read after its entry (the half-second morph out of the
// voice, the label crossfade), and the empty breath after the fade out is long enough that the
// next loop reads as a new take, not a flicker.
inline constexpr std::array<SampleStage, 4> kStages = {
    {{"recording", 0.0}, {"processing", 4.8}, {"transcribing", 6.6}, {"done", 8.4}}};

// One word of the sample: when it starts and how long it lasts (seconds), and its peak RMS.
struct SampleWord {
    double start;
    double length;
    double peak;
};

// The words, as _sample_words(3) lays them out. Python draws them with random.Random(3) at
// import; the table is data/sample_voice.json (tools/gen/overlay_tables.py), parsed once.
// Throws std::runtime_error if the embedded table is malformed.
const std::vector<SampleWord>& sample_words();

// The sample voice's RMS level t seconds into the loop (t is taken modulo kLoopS): a
// faint hum between words and sin(pi * progress) ** 0.7 across each word.
double sample_level(double t);

} // namespace fv
