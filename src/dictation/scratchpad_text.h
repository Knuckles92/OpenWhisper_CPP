#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// The text rules of the Scratchpad window (ui_qt/widgets/scratchpad.py): how a dictation is
// spaced into the text around the cursor, how a block is appended, and the word count line.
// Text is UTF-8; positions are the UTF-16 units Qt's text cursor counts. Every function here is
// thread-safe.
namespace fv::dictation {

// _CONTEXT_BEFORE, _CONTEXT_AFTER: UTF-16 units of text read on either side of the cursor.
inline constexpr std::int64_t SCRATCHPAD_CONTEXT_BEFORE = 1000;
inline constexpr std::int64_t SCRATCHPAD_CONTEXT_AFTER = 200;

// _utf16_slice: the text between two UTF-16 positions of `text` (negative positions count as
// 0). Half of a surrogate pair cut by a position is dropped, as decoding with errors="ignore"
// drops it.
std::string utf16_slice(std::string_view text, std::int64_t start, std::int64_t end);

// _spaced: `text` with the spaces it needs to sit between `before` and `after`. A space goes in
// front unless `before` is empty or ends in one of ([{"'“‘/-— newline, tab or space, or `text`
// starts with whitespace or one of .,;:!?)]}%…'"’”; a space goes behind when `after` starts
// with a letter or digit and `text` does not end with whitespace.
std::string spaced(std::string_view before, std::string_view text, std::string_view after);

// append_block: what to insert at the end of `existing` to add `text` after everything else:
// the stripped text, after a blank line (one newline when `existing` already ends with one, none
// when `existing` is blank).
std::string append_block(std::string_view existing, std::string_view text);

// _show_word_count: "" for no words, "1 word", else "{count:,} words" (words as str.split()).
std::string word_count_text(std::string_view text);

} // namespace fv::dictation
