#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Whole-word term replacement shared by dictation and Meeting Mode (services/vocabulary.py).
// Every function is pure and thread-safe.
namespace fv::text {

// `text` for caseless matching, and for each code point of `folded` the index of the code point
// of `text` it came from. "İ" folds as "I", and combining dots U+0307 right after an "i" are
// dropped, so a typed "i" matches a dotted capital; a letter whose fold is longer ("ß" -> "ss")
// gives several folded code points the same origin.
struct FoldedText {
    std::string folded;              // UTF-8
    std::vector<std::size_t> origin; // code point index into the original text, per code point
};
FoldedText fold_with_origin(std::string_view text);

// Whole-word, case-insensitive replacements in a single pass. `rules` maps a term, in any case,
// to its replacement, in insertion order as Python's mapping: terms that fold alike count once,
// the first one winning (for an exact duplicate too). One pass means a replacement is never
// matched by another rule, and longer terms are tried first so a phrase wins over one of its
// words. A match that starts or ends inside a character that folded to several is skipped.
// Runs in time linear in the text.
std::string replace_terms(std::string_view text,
                          const std::vector<std::pair<std::string, std::string>>& rules);

} // namespace fv::text
