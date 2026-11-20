#pragma once

#include <cstddef>
#include <exception>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>

#include "base/json.h"

// Python's view of the decoded JSON the host reads (services/remote_asr/host.py's checks).
namespace fv::remote_host::detail {

// bool(value): null, false, 0, 0.0, "", [] and {} are false; NaN is true.
bool truthy(const Json& value);

// str(value): a string as it is, None/True/False, an int in decimal, a float as repr, and a list
// or dict as Python's repr of it.
std::string py_str(const Json& value);

// repr(value), as an f-string's !r writes it ("'bogus'", "None", "[1, 2]").
std::string py_repr(const Json& value);

// isinstance(value, str) and value: a non-empty JSON string.
bool nonempty_string(const Json& value);

// set(header) - allowed is not empty: the object has a key outside `allowed`. A value that is
// not an object has none.
bool has_other_keys(const Json& header, std::initializer_list<std::string_view> allowed);

// header.get(key), or null when the header has no such key or is not an object.
const Json& get(const Json& header, std::string_view key);

// `value` is a JSON string equal to `text`.
bool is_text(const Json& value, std::string_view text);

// str(exc) or type(exc).__name__: what() when it says something, else the Python class name
// pyport errors carry, else "RuntimeError".
std::string error_text(const std::exception& error);

// Bytes held in a std::string, and back.
std::span<const std::byte> bytes_of(std::string_view text);
std::string_view chars_of(std::span<const std::byte> bytes);

} // namespace fv::remote_host::detail
