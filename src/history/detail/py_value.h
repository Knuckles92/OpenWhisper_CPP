#pragma once

#include <string>
#include <string_view>

#include "base/json.h"

// Python's reading of loosely typed dict values, for the history modules that take dicts.
namespace fv::history_detail {

// bool(value).
bool truthy(const Json& value);
// str(value): text as is, None, True and False, ints, floats as repr(), containers as JSON.
std::string py_str(const Json& value);
// record.get(key), None when the record is not an object or has no such key.
const Json& field(const Json& record, std::string_view key);
// str(record.get(key) or fallback).
std::string text_or(const Json& record, std::string_view key, std::string_view fallback);

} // namespace fv::history_detail
