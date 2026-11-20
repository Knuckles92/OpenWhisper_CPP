#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

// Python idioms the review, report, repair and export code shares.
namespace fv::meeting_state::detail {

// (value or ""): text as is, "" for a falsy value; anything else is the AttributeError Python
// raises when it calls a str method on it.
std::string text_or_empty(const Json& value);

// str.replace(old, new): every occurrence, left to right.
std::string replace_all(std::string text, std::string_view from, std::string_view to);

// str.splitlines(): \n, \r, \r\n, \v, \f, \x1c, \x1d, \x1e, \x85, U+2028 and U+2029 end a line;
// the ends are dropped and a final line ending adds no empty line.
std::vector<std::string> splitlines(std::string_view text);

// sep.join(parts).
std::string join(const std::vector<std::string>& parts, std::string_view separator);

// value.get(key, []) iterated: the member's items, none when it is missing; a member that is
// not a list (None included) is Python's TypeError.
const Json& list_member(const Json& object, std::string_view key);

// A mapping Python reads with .get: an object as is; anything else is AttributeError.
const Json& mapping(const Json& value);
Json& mapping(Json& value);

// isinstance(value, (int, float)) and not isinstance(value, bool).
bool is_real_number(const Json& value);

// parse_meeting_time(value).timestamp(): an aware value's instant, a naive one read as local
// wall time in `zone` (fold=0); nullopt when the value does not parse.
std::optional<double> meeting_timestamp(const Json& value, const std::chrono::time_zone& zone);

} // namespace fv::meeting_state::detail
