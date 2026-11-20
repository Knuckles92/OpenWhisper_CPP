#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "base/json.h"

// Python's view of the loosely typed dicts the meeting code passes around.
namespace fv::meeting::detail {

// bool(value): null, false, 0, 0.0, "", [] and {} are false.
bool truthy(const Json& value);

// value.get(key): the member, or a null when it is missing or `object` is not an object.
const Json& get(const Json& object, std::string_view key);

// str(value) of a decoded JSON value, as Python prints it ("None", "True", "1.0", "['a', 1]").
std::string py_str(const Json& value);

// str(value or ""): the text of a truthy value, else "".
std::string str_or_empty(const Json& value);

// int(value or 0), raising py::ValueError or py::TypeError as Python does.
std::int64_t int_or_zero(const Json& value);

// list(value or []): an array as is, a string's code points, an object's keys; another truthy
// value is Python's TypeError.
Json list_or_empty(const Json& value);

} // namespace fv::meeting::detail
