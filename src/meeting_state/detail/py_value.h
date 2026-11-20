#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "base/json.h"

// Python's view of loosely typed JSON values, for code ported from dict-handling Python.
namespace fv::meeting_state::detail {

// bool(value): null, false, 0, 0.0, "", [] and {} are false.
bool truthy(const Json& value);

// value.get(key): the member, or a null when it is missing or `object` is not an object.
const Json& get(const Json& object, std::string_view key);
bool has(const Json& object, std::string_view key);

// object.get(key, fallback) for a field read as text: the member when it is a string, else
// the fallback (a member of another type counts as missing).
std::string string_or(const Json& object, std::string_view key, std::string_view fallback);

// str(value) and repr(value) of a JSON value as Python prints the decoded object.
std::string py_str(const Json& value);
std::string py_repr(const Json& value);

// list(value): an array as is, a string's code points, an object's keys; anything else is
// Python's TypeError.
Json py_list(const Json& value);

// int(value), raising py::TypeError or py::ValueError as Python does.
std::int64_t py_int_or_throw(const Json& value);

// The KeyError d[key] raises.
[[noreturn]] void key_error(std::string_view key);

// d[key] on a JSON object: KeyError when missing, TypeError when `object` is not one.
const Json& at(const Json& object, std::string_view key);
Json& at(Json& object, std::string_view key);

} // namespace fv::meeting_state::detail
