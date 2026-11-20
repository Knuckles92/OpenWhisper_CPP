#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "focus/snapshot.h"

namespace fv::focus {

// The prompt carries less than a capture holds: small local models (Ollama gives a model 4096
// tokens unless told otherwise) also need room for the instructions, the rules, the dictionary
// and the transcript. Code points.
inline constexpr std::size_t PROMPT_BEFORE_CHARS = 600;
inline constexpr std::size_t PROMPT_AFTER_CHARS = 200;
inline constexpr std::size_t PROMPT_SELECTED_CHARS = 400;

// The cleanup-prompt block describing the text around the caret, or "". Only on-screen text and
// the app's name (the site's, in a browser): never a window title. Thread-safe.
std::string prompt_block(const std::optional<FocusSnapshot>& snapshot);

// The last `limit` code points of text, starting at a word when the cut lands early in one,
// after "…"; text itself when it is short enough.
std::string prompt_tail(std::string_view text, std::size_t limit);
// The first `limit` code points of text, ending at a word when the cut lands late in one,
// before "…"; text itself when it is short enough.
std::string prompt_head(std::string_view text, std::size_t limit);

} // namespace fv::focus
