#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fv {

// Times are seconds from the start of the audio.
struct Word {
    std::string text;
    double start = 0.0;
    double end = 0.0;

    bool operator==(const Word&) const = default;
};

struct Segment {
    std::string text;
    double start = 0.0;
    double end = 0.0;

    bool operator==(const Segment&) const = default;
};

struct Transcript {
    std::string text;
    std::vector<Word> words;
    std::vector<Segment> segments;
    // Seconds of audio the runtime had decoded when it produced this.
    double audio_processed = 0.0;
};

// A segment ends at sentence punctuation, or after this many words.
inline constexpr std::size_t kSegmentMaxWords = 24;

// Groups word timings into sentence-like segments. Without word timings the
// whole text becomes one segment spanning `duration` seconds, or none when the
// text is blank.
std::vector<Segment> segments_from_words(std::string_view text, std::span<const Word> words,
                                         double duration);

} // namespace fv
