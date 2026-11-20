#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "pyport/errors.h"

// meeting/corrections.py: reversible transcript corrections a human attaches to the meeting.
// A correction is a human-authored user_notes item whose data.kind is "term_correction"
// (selected_text -> replacement everywhere), "occurrence_correction" (one occurrence in one
// cited segment) or "agent_insight" (a free clarification). Raw ASR text is never rewritten:
// readers apply the rules, so removing a note reverses only its effect.
// Not here: repository_term_rules (M7.7) and guidance_prompt (M8).
// Affinity: thread-safe (pure functions).
namespace fv::meeting_state {

using Json = fv::Json;

// Longest selected_text or replacement a correction may carry, in code points.
inline constexpr std::size_t MAX_TERM_CHARS = 120;
// Bounds of the vocabulary hint.
inline constexpr std::size_t VOCABULARY_MAX_TERMS = 12;
inline constexpr std::size_t VOCABULARY_MAX_CHARS = 160;

// Lower-cased misheard term -> replacement, in Python's dict order (a later note for the same
// term replaces the value in place).
using TermRules = std::vector<std::pair<std::string, std::string>>;

// One validated one-occurrence correction.
struct OccurrenceRule {
    std::string selected_text; // stripped
    std::string replacement;   // stripped
    std::int64_t occurrence_index = 0;
    std::string base_fingerprint;

    friend bool operator==(const OccurrenceRule&, const OccurrenceRule&) = default;
};
// Rules by their single evidence segment id, each list in note order.
using ScopedRules = std::map<std::string, std::vector<OccurrenceRule>, std::less<>>;

// The selected passage changed before its correction was committed (a ValueError in Python).
class StaleCorrectionSelection : public py::ValueError {
public:
    explicit StaleCorrectionSelection(const std::string& segment_id)
        : py::ValueError("StaleCorrectionSelection", segment_id) {}
};

// "fnv1a64:%016x": FNV-1a 64 over the UTF-8 bytes, shared with the dashboard renderer.
std::string segment_fingerprint(std::string_view text);
// Text matching fnv1a64:[0-9a-f]{16} exactly.
bool valid_segment_fingerprint(const Json& value);

// A note by a user, or by the spoken "fix_transcript" command (author system/voice_command with
// data.source voice_command).
bool is_correction_author(const Json& item);

// Term rules from live, correction-authored term_correction items whose stripped terms are
// 1..MAX_TERM_CHARS code points; `items` is any JSON list (other entries are skipped).
TermRules term_rules_from_items(const Json& items);
// From a MeetingState::to_json() snapshot's user_notes.
TermRules term_rules(const Json& state);
// Validated one-occurrence corrections of a snapshot, grouped by evidence segment.
ScopedRules occurrence_rules(const Json& state);

// Whole-word, case-insensitive term corrections in a single pass (text::replace_terms): a
// replacement is never matched again, and a phrase wins over one of its words.
std::string correct_text(std::string_view text, const TermRules& rules);

// Meeting-wide terms first, then this segment's one-occurrence edits. Every occurrence index
// counts matches of (?<!\w)<term>(?!\w) (Unicode \w, IGNORECASE) in the globally corrected
// text; an edit whose base fingerprint differs from that text is suspended, and one that
// overlaps an earlier edit is skipped.
std::string correct_segment_text(std::string_view text, std::string_view segment_id,
                                 const TermRules& rules, const ScopedRules& scoped);

// Distinct corrected spellings (compared lower-cased), oldest rule first, at most max_terms and
// max_chars code points counting ", " between terms.
std::vector<std::string> vocabulary_terms(const TermRules& rules,
                                          std::size_t max_terms = VOCABULARY_MAX_TERMS,
                                          std::size_t max_chars = VOCABULARY_MAX_CHARS);
// "Anthropic, Claude." or "". Parakeet takes no text prompt, so nothing consumes it yet.
std::string vocabulary_hint(const TermRules& rules);

} // namespace fv::meeting_state
