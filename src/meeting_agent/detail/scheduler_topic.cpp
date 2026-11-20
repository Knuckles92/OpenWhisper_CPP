// The early trigger: a topic shift between the last two 60-second transcript windows, judged
// by the semantic TopicJudge when the engine supplies one, else by content-word Jaccard.

#include <algorithm>
#include <cstddef>
#include <exception>
#include <iterator>
#include <string>

#include "meeting_agent/detail/scheduler_support.h"
#include "meeting_agent/scheduler.h"
#include "pyport/text.h"

namespace fv::meeting_agent {

namespace {

// Only words of length >= 4 are compared, so shorter function words never reach this list.
constexpr std::string_view kStopwords[] = {
    "that",    "this",  "with",    "have",      "will",     "from",      "they",   "been",  "were",
    "what",    "when",  "your",    "just",      "like",     "know",      "think",  "about", "there",
    "their",   "would", "could",   "should",    "because",  "really",    "going",  "yeah",  "okay",
    "right",   "well",  "then",    "them",      "than",     "some",      "also",   "into",  "over",
    "only",    "very",  "more",    "most",      "other",    "which",     "while",  "where", "after",
    "before",  "thing", "things",  "something", "actually", "basically", "little", "kind",  "sort",
    "want",    "need",  "make",    "made",      "doing",    "done",      "gonna",  "mean",  "maybe",
    "here",    "these", "those",   "anyway",    "again",    "still",     "said",   "says",  "talk",
    "talking", "look",  "looking", "good",      "great",    "sure",      "does",   "much",  "many",
    "even",    "back",  "being",   "each"};

bool is_stopword(std::string_view word) {
    return std::ranges::find(kStopwords, word) != std::end(kStopwords);
}

// Seconds of meeting the two windows cover together, and each one's length.
constexpr double kWindowsS = 120.0;
constexpr double kWindowS = 60.0;

} // namespace

std::set<std::string, std::less<>> content_words(std::string_view text) {
    // [a-z']+ over text.lower(): every other code point (any byte of a multi-byte one included)
    // ends a word.
    const std::string lowered = py::lower(text);
    std::set<std::string, std::less<>> words;
    std::string word;
    const auto flush = [&] {
        if (word.size() >= 4 && !is_stopword(word)) {
            words.insert(word);
        }
        word.clear();
    };
    for (const char c : lowered) {
        if ((c >= 'a' && c <= 'z') || c == '\'') {
            word.push_back(c);
        } else {
            flush();
        }
    }
    flush();
    return words;
}

bool CheckpointScheduler::detect_topic_shift() {
    const double now_mono = mono();
    if (now_mono - last_shift_check_mono_ < options_.shift_check_spacing_s) {
        return false;
    }
    last_shift_check_mono_ = now_mono;

    if (!host_.has_meeting_clock()) {
        return false;
    }
    const double now_s = host_.meeting_clock_s();
    if (now_s < kWindowsS) {
        return false;
    }
    std::vector<Json> recent;
    try {
        recent = host_.get_transcript(now_s - kWindowsS);
    } catch (const std::exception& error) {
        detail::scheduler_log.error_with(error, "Topic-shift transcript fetch failed");
        return false;
    }
    const double cut_s = now_s - kWindowS;
    std::string older_text;
    std::string newer_text;
    bool older_any = false;
    bool newer_any = false;
    for (const auto& segment : recent) {
        const bool older = detail::row_start_s(segment) < cut_s;
        std::string& into = older ? older_text : newer_text;
        bool& any = older ? older_any : newer_any;
        if (any) {
            into += ' ';
        }
        into += detail::row_text(segment);
        any = true;
    }
    const auto older_words = content_words(older_text);
    const auto newer_words = content_words(newer_text);
    if (older_words.size() < SHIFT_MIN_WORDS || newer_words.size() < SHIFT_MIN_WORDS) {
        return false;
    }
    if (const auto semantic = semantic_topic_shift(older_text, newer_text)) {
        return *semantic;
    }
    std::size_t shared = 0;
    for (const auto& word : older_words) {
        shared += newer_words.contains(word) ? 1 : 0;
    }
    const std::size_t all = older_words.size() + newer_words.size() - shared;
    const double jaccard = all > 0 ? static_cast<double>(shared) / static_cast<double>(all) : 1.0;
    if (jaccard < SHIFT_JACCARD_THRESHOLD) {
        detail::scheduler_log.info("Topic shift detected (jaccard={}); firing checkpoint early",
                                   detail::fixed(jaccard, 3));
        return true;
    }
    return false;
}

std::optional<bool> CheckpointScheduler::semantic_topic_shift(const std::string& older,
                                                              const std::string& newer) {
    // Runs on the worker, outside the store's lock, bounded by the judge's own timeout; any
    // failure is "no answer", so a remote outage can never stall checkpoints.
    if (!topic_judge_) {
        return std::nullopt;
    }
    std::optional<double> probability;
    try {
        probability = topic_judge_(older, newer);
    } catch (const std::exception& error) {
        detail::scheduler_log.error_with(error, "Semantic topic-shift judge failed; using Jaccard");
        return std::nullopt;
    } catch (...) {
        detail::scheduler_log.error("Semantic topic-shift judge failed; using Jaccard");
        return std::nullopt;
    }
    if (!probability) {
        return std::nullopt;
    }
    const bool shifted = *probability >= SHIFT_SEMANTIC_THRESHOLD;
    if (shifted) {
        detail::scheduler_log.info("Topic shift detected (semantic p={}); firing checkpoint early",
                                   detail::fixed(*probability, 2));
    }
    return shifted;
}

} // namespace fv::meeting_agent
