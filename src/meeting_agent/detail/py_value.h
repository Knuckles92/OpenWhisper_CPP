#pragma once

#include <string>
#include <string_view>

#include "base/json.h"

// Python's view of JSON values, as the prompt builders and the tool policy need it: str() and
// repr() of a value, truthiness, and dict.get with and without a default.
namespace fv::meeting_agent::detail {

// str(value): None, True and False by name, floats as repr(), strings as themselves, lists and
// dicts as Python prints them ("['a', 1]", "{'k': None}").
std::string py_str(const Json& value);
// repr(value): as py_str, except that a string is quoted as Python quotes it.
std::string py_repr(const Json& value);
// bool(value).
bool py_truthy(const Json& value);
// list(value): an array as is, a string's code points, an object's keys; another truthy value
// is Python's TypeError. `value` is first read as (value or []), so a falsy value is empty.
Json py_list_or_empty(const Json& value);

// value.get(key) of a dict: null for a missing key or a value that is not an object.
const Json& get(const Json& object, std::string_view key);
// value.get(key, default): `fallback` only when the key is missing (a stored null stays null).
const Json& get_or(const Json& object, std::string_view key, const Json& fallback);
// (value.get(key) or {}) as an object; anything falsy or not an object reads as empty.
const Json& get_object(const Json& object, std::string_view key);
// str(value.get(key) or "").
std::string str_or_empty(const Json& value);

// value == text for a JSON string.
bool is_text(const Json& value, std::string_view text);

} // namespace fv::meeting_agent::detail
