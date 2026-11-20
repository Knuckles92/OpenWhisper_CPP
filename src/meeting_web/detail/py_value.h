#pragma once

#include <string>
#include <string_view>

#include "base/json.h"

// Python's view of a JSON value, for the few places the dashboard code calls str(), repr() or
// bool() on whatever a client or a stored row holds.
namespace fv::meeting_web::detail {

// bool(value): null, false, 0, 0.0, "", [] and {} are false.
bool truthy(const Json& value);

// repr(value) of the Python object json.loads would give: None, True, 3, 1.5, 'text', [1, 'a'],
// {'k': None}.
std::string py_repr(const Json& value);

// str(value): a string as it is, anything else as repr().
std::string py_str(const Json& value);

// `a or b`: a when it is truthy, else b.
const Json& py_or(const Json& a, const Json& b);

// The value of `key` in an object, or null (dict.get).
const Json& get(const Json& object, std::string_view key);

} // namespace fv::meeting_web::detail
