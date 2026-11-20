#pragma once

#include <exception>
#include <string>
#include <string_view>

#include "base/json.h"

// Python's view of JSON values, where the automation modules lean on it.
namespace fv::automation::detail {

// bool(value): null, false, 0, 0.0, "", [] and {} are false.
bool truthy(const Json& value);

// str(value) of a scalar: a string itself, an int in decimal, a float as repr(), True/False,
// None. A list or object is written as compact JSON, which differs from Python's repr (no
// payload builder is given one).
std::string py_str(const Json& value);

// Whether every string in `value`, keys included, is well-formed UTF-8. A surrogate encoded in
// three bytes (how json.loads gives a lone "\ud83d") is not: Python cannot encode it as UTF-8.
bool encodes_as_utf8(const Json& value);
bool encodes_as_utf8(std::string_view text);

// The value at `key` of an object, or null when `item` is not an object or lacks the key.
const Json& get(const Json& item, std::string_view key);

// What a log line may say about an exception: its type only ("TypeError" for the Python
// exception a pyport error stands in for), never its text, which can quote dictated words.
std::string error_type(const std::exception& error);

} // namespace fv::automation::detail
