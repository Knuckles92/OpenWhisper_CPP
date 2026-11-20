#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <QJsonObject>

#include "base/json.h"

// Python's view of the loosely typed payload dicts the Meeting Mode tab receives.
namespace fv::ui_meeting::detail {

// bool(value): null, false, 0, 0.0, "", [] and {} are false; NaN is true.
bool truthy(const Json& value);

// value.get(key): the member, or a null when it is missing or `object` is not an object.
const Json& get(const Json& object, std::string_view key);

// str(value) as Python prints a decoded JSON value ("None", "True", "1.0", "['a', 1]").
std::string pyStr(const Json& value);

// str(value or ""): the text of a truthy value, else "".
std::string strOrEmpty(const Json& value);

// int(value or 0), raising py::ValueError, py::TypeError or py::OverflowError as Python does.
std::int64_t intOrZero(const Json& value);

// list(value or []): an array as it is, a string's characters, an object's keys; py::TypeError
// for a number or a bool, which list() cannot iterate.
Json listOrEmpty(const Json& value);

// dict(value or {}): an object as it is, a list of (key, value) pairs as the object they make;
// py::TypeError or py::ValueError where dict() raises them (a number, a string, an element that
// is not a pair).
Json dictOrEmpty(const Json& value);

// A float, an int or a bool the way isinstance(value, (int, float)) accepts it (a bool is an
// int in Python); nullopt for anything else.
std::optional<double> pythonNumber(const Json& value);

// The object as JSON, numbers as QJsonDocument writes them (an integral double reads back as
// an integer, which every reader here takes the same way).
Json toJson(const QJsonObject& object);

} // namespace fv::ui_meeting::detail
