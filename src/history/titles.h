#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "base/json.h"

// services/titles.py: the display-title contract shared by History, the meeting dashboard and
// the MCP tools.
// Affinity: thread-safe (pure functions).
namespace fv {

inline constexpr std::size_t MAX_TITLE_LENGTH = 200; // code points
inline constexpr std::string_view TITLE_ERROR_MESSAGE =
    "Supply a title of 1–200 characters without control characters.";

// "invalid_text" for blank text or a control character (below U+0020, or U+007F),
// "text_too_long" past MAX_TITLE_LENGTH code points, nullopt for a valid title.
std::optional<std::string> title_error(std::string_view value);
// Anything but a JSON string is "invalid_text".
std::optional<std::string> title_error(const Json& value);

// The title stripped of surrounding whitespace. Throws py::ValueError(TITLE_ERROR_MESSAGE) for
// an invalid one.
std::string normalize_title(std::string_view value);
std::string normalize_title(const Json& value);

} // namespace fv
