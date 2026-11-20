#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "focus/snapshot.h"

namespace fv::focus {

// text adjusted to continue what is already before the caret. Deterministic and only for a
// known caret: a space where the text would otherwise touch the word before it, a lowercase
// first word when the sentence goes on, and no closing period when more of it follows.
// Python's Unicode rules throughout (str.isupper, casefold, re's \w). Thread-safe.
std::string join_with_context(std::string_view text, const std::optional<TextContext>& context);

// html, the rich-text twin of text, given text's join. The edits are decided on the plain text
// and replayed on the fragment's first and last text outside its tags, so a rich-text paste
// reads like the plain one; a space added at an edge is "&nbsp;", because rich-text editors may
// collapse ordinary whitespace there. A first word the markup doesn't start with keeps its
// capital. Thread-safe.
std::string join_html_with_context(std::string_view html, std::string_view text,
                                   const std::optional<TextContext>& context);

} // namespace fv::focus
