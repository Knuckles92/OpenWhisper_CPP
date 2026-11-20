#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "bench/ami.h"

// benchmarks/meeting_mode/metrics.py: time-aware word-error metrics for long multiparty
// meetings, and the scoring every other benchmark reuses. Reports are Json with Python's keys in
// Python's order.
namespace fv::bench {

// Levenshtein operation counts against a reference token sequence.
struct EditCounts {
    std::size_t substitutions = 0;
    std::size_t deletions = 0;
    std::size_t insertions = 0;

    std::size_t errors() const { return substitutions + deletions + insertions; }
    EditCounts& operator+=(const EditCounts& other);
    friend bool operator==(const EditCounts&, const EditCounts&) = default;
};

// normalize_tokens: NFKC, lower case, ’ and ` read as ', AMI's acronym underscores ("A_S_R_")
// removed, then the runs of \w+(?:'\w+)? (Unicode \w). Fillers and numbers stay.
std::vector<std::string> normalize_tokens(std::string_view text);

// edit_counts: exact word-level Levenshtein counts with two rows; ties go substitution,
// deletion, insertion in that order, so the split is deterministic and the total exact.
EditCounts edit_counts(std::span<const std::string> reference,
                       std::span<const std::string> hypothesis);

// score_text: ordered words scored after normalize_tokens. A silent reference keeps its
// insertions and has no WER.
struct TextScore {
    EditCounts counts;
    std::size_t words = 0;
    std::size_t hypothesis_words = 0;
    std::optional<double> wer;

    std::size_t errors() const { return counts.errors(); }
    // {substitutions, deletions, insertions, words, hypothesis_words, errors, wer}.
    Json to_json() const;
};
TextScore score_text(std::string_view reference, std::string_view hypothesis);

// reference_overlap_stats: {reference_words, overlapping_reference_words, overlap_rate}, a word
// overlapping when a word of another speaker (both of positive length) overlaps it in time.
Json reference_overlap_stats(std::span<const ReferenceWord> reference_words);

// A hypothesis segment on the meeting clock.
struct TimedText {
    double start_s = 0.0;
    double end_s = 0.0;
    std::string text;
};

// score_timed_transcript (tcWER): fixed meeting-time windows of window_s, so a duplicated or
// missing passage cannot shift the alignment of the rest of the meeting. Reference words fall
// in a window by start time, hypothesis segments by max(evaluation start, start); segments
// wholly outside the reference's span are left out and their words counted as
// out_of_reference_hypothesis_words. Throws std::invalid_argument for a window_s <= 0.
Json score_timed_transcript(std::span<const ReferenceWord> reference_words,
                            std::span<const TimedText> hypothesis_segments,
                            double window_s = 300.0);

// The TimedText of each {start_s, end_s, text} object (missing or null values read as 0 / "").
std::vector<TimedText> timed_texts(const Json& segments);

// aggregate_scores: micro-averaged counts and rates over timed scores, and macro_wer.
Json aggregate_scores(std::span<const Json> scores);

} // namespace fv::bench
