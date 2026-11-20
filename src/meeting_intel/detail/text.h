#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

#include "base/json.h"

// What recall, the knowledge folder and history search share: Python's truthiness and str() of
// a stored value, the evidence-id scrub and the clip every snippet goes through. Lengths are in
// code points, as Python's.
namespace fv::meeting_intel::detail {

// bool(value): null, false, 0, "" and empty containers are false.
bool truthy(const Json& value);
// str(value): a string as it is, null as "None", a bool as "True"/"False", a number as Python
// prints it, a container as its JSON text.
std::string py_str(const Json& value);
// str(value or ""): "" for anything falsy, else py_str.
std::string text_or_empty(const Json& value);

// _sanitize: every \bsg_[0-9a-fA-F]{6,}\b becomes "[id]", so ids that look like live evidence
// ids never reach a model.
std::string sanitize(std::string_view text);
// _clip: sanitized and stripped; above `max_chars` the first max_chars - 1 code points,
// right-stripped, then "…".
std::string clip(std::string_view text, std::size_t max_chars);

// The exception's class name, for log lines that must not quote the message (it can hold
// transcript or file text): a pyport error's Python name, else the C++ type.
std::string type_name(const std::exception& error);

// int(limit) clamped to 1..max_limit, `fallback` when int() raises TypeError or ValueError.
// int() of an infinite float raises OverflowError, which Python does not catch here: that
// throws py::PyError("OverflowError").
std::int64_t clamp_limit(const Json& limit, std::int64_t fallback, std::int64_t max_limit);

} // namespace fv::meeting_intel::detail
