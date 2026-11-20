#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "net/types.h"
#include "pyport/datetime.h"

// Query parameters as FastAPI validates them with pydantic 2 (lax mode, from text). A nullopt
// result is a validation error, which the History API answers 422.
namespace fv::agentapi::detail {

// The value FastAPI reads for a scalar parameter: the last one of that name (Starlette's
// QueryParams keeps the last in its dict).
std::optional<std::string> last_value(const net::Query&, std::string_view name);

// bool: true, 1, yes, on, t, y (false, 0, no, off, f, n) in any case, nothing around them.
std::optional<bool> parse_bool(std::string_view);

// int: Unicode whitespace around it, one sign, ASCII digits with single '_' between them, and a
// fraction of zeros ("20.0"). A value beyond int64 is refused; every caller bounds it anyway.
std::optional<std::int64_t> parse_int(std::string_view);

// float: Unicode whitespace around it, then Python's float() syntax in ASCII ("1e3", "1_0",
// "inf", "nan").
std::optional<double> parse_float(std::string_view);

// datetime: "YYYY-MM-DD" (midnight, naive), or that with 'T', 't', ' ' or '_' and
// "HH:MM[:SS[.f]]" (a '.' or ',' fraction, digits past the sixth dropped) and an optional "Z" or
// "+HH:MM"/"+HHMM" offset under 24 h; or a Unix time in seconds, or in milliseconds beyond
// 2e10, with an optional fraction, which is UTC. Nothing may surround it.
std::optional<py::DateTime> parse_datetime(std::string_view);

// value.astimezone(UTC): the same instant at offset 0. Throws py::OverflowError("date value out
// of range") when that falls outside years 1 to 9999. `value` must be aware.
py::DateTime to_utc(const py::DateTime& value);

} // namespace fv::agentapi::detail
