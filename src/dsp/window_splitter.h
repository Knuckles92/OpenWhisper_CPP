#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fv::dsp {

// Decoding windows are 16 kHz mono and at most 30 s long. A longer stream is cut after the
// quietest 0.1 s block between 24 s and 30 s, so a cut lands in a pause rather than mid-word.
inline constexpr std::int64_t SAMPLE_RATE = 16000;
inline constexpr std::int64_t MAX_SAMPLES = 30 * SAMPLE_RATE;
inline constexpr std::int64_t MIN_SAMPLES = 24 * SAMPLE_RATE;
inline constexpr std::int64_t SPLIT_BLOCK_SAMPLES = 1600;

// Where the first window of audio ends: the whole of it below MAX_SAMPLES, else one block past
// the block of [MIN_SAMPLES, MAX_SAMPLES) with the lowest mean square (the first one on a tie),
// computed in float32 as numpy does.
std::size_t split_point(std::span<const float> audio);

struct Window {
    double start_s = 0.0; // where samples[0] sits in the stream, in seconds
    std::vector<float> samples;
};

// Cuts a 16 kHz s16 stream that arrives in pieces into the windows split_point gives the whole
// stream: a cut looks only at the first MAX_SAMPLES pending, so the windows depend on the samples
// alone, never on how they were divided into pieces. Samples become float as s16 / 32768.
// Affinity: one thread at a time.
class WindowSplitter {
public:
    // Adds samples; returns the windows they complete, in order.
    std::vector<Window> push(std::span<const std::int16_t> s16_16k);
    // The final partial window, if any audio is left; the splitter is then empty.
    std::vector<Window> finish();
    // Samples pushed so far.
    std::int64_t total() const;
    // Samples already returned in windows.
    std::int64_t offset() const;

private:
    std::vector<float> pending_;
    std::int64_t offset_ = 0;
};

} // namespace fv::dsp
