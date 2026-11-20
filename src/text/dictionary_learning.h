#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The pure half of learning dictionary words from corrections (services/dictionary.py
// _tokens ... _correction). Reading the field again after a paste, counting sightings and saving
// the word belong to fv_dictation. Every function is pure and thread-safe.
namespace fv::text {

// Python's _tokens: whitespace-separated pieces with .,!?;:"“”'‘’()[]{}<>«»… stripped from both
// ends, empty pieces dropped.
std::vector<std::string> correction_tokens(std::string_view text);

// Words too common to be a personal spelling, even when the user fixed one (COMMON_WORDS,
// casefolded). `word` is compared as given.
bool is_common_word(std::string_view word);

// (pasted word, the user's word) when `reread` (the text around the caret read a few seconds
// after the paste) still holds the pasted text with exactly one pasted word swapped for one other
// word, with at least two unchanged words to anchor it and at least three pasted words. Text
// typed before or after the pasted span is ignored. The new word must be a word of three or more
// characters with a letter, not a common word, and differ by more than case, apostrophes or
// hyphens. Python's learned_correction returns the second half; _correction returns the pair.
std::optional<std::pair<std::string, std::string>> learned_correction(std::string_view pasted,
                                                                      std::string_view reread);

// Whether a correction put back a word AI cleanup had changed: `restored` was in the text before
// cleanup and `replaced` was not, so cleanup brought it in. nullopt or "" before cleanup (cleanup
// changed nothing) is never a revert.
bool reverted_cleanup(std::optional<std::string_view> before_cleanup, std::string_view replaced,
                      std::string_view restored);

} // namespace fv::text
