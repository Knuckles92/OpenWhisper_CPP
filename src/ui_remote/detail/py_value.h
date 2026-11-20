#pragma once

#include <string>

#include "base/json.h"

// Python's truth and str() over the JSON values the remote engine service reports, for the
// wording helpers that read them as OpenWhisper's page does.
namespace fv::ui_remote::detail {

bool truthy(const Json& value);
// str(value): strings as they are, True/False/None, ints, floats as repr(), containers as repr().
std::string py_str(const Json& value);
// str(value or ""): "" for a falsy value.
std::string py_str_or_empty(const Json& value);
// value.get(key) of a dict; null for anything that is not an object or lacks the key.
const Json& get(const Json& value, const char* key);

} // namespace fv::ui_remote::detail
