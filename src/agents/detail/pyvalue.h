#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "process/launcher.h"

// Python's view of parsed JSON values and of child output, as the agent code reads them.
namespace fv::agents::detail {

// bool(value).
bool truthy(const Json& value);
// str(value): a string as it is, None/True/False, numbers as repr, containers as repr.
std::string py_str(const Json& value);
// value.get(key) of a dict; null for a missing key or a value that is not an object.
const Json& get(const Json& object, std::string_view key);
// The items `for x in value` visits: a list's items, a dict's keys, a string's characters;
// empty for null and other falsy values. Throws py::TypeError for anything else truthy (a
// number, True), as iterating it would.
std::vector<Json> iterate(const Json& value);
// `needle in value`: a dict's key, a list's item, a substring. Throws py::TypeError otherwise.
bool contains(const Json& value, std::string_view needle);
// Python's isinstance(value, int) for a JSON id (a bool is an int).
bool is_py_int(const Json& value);

// bytes.decode("utf-8", errors="replace"): each maximal invalid subpart becomes one U+FFFD.
std::string decode_utf8_replace(std::string_view bytes);
// True when `bytes` is UTF-8 Python's strict codec accepts.
bool is_valid_utf8(std::string_view bytes);
// A text-mode read: decoded with errors replaced, then "\r\n" and "\r" read as "\n".
std::string text_mode(std::string_view bytes);
// Universal newlines only ("\r\n" and "\r" as "\n"), for text already known to be UTF-8.
std::string universal_newlines(std::string_view text);

// str.splitlines(): splits on \n, \r, \r\n, \v, \f, \x1c-\x1e, U+0085, U+2028 and U+2029;
// a final line break adds no empty line.
std::vector<std::string> splitlines(std::string_view utf8);

// A ByteSink that hands `sink` each line without its ending, as a text-mode pipe reads them
// ("\n", "\r\n" or a lone "\r"), decoded as decode_utf8_replace, then the unterminated rest; and
// `on_end` once the pipe is done.
ByteSink lines(std::function<void(std::string line)> sink, std::function<void()> on_end = {});

} // namespace fv::agents::detail
