#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "pyport/datetime.h"

// What the MCP SDK's pydantic (2.13, pydantic-core's jiter and speedate) does to the requests
// the in-app MCP server takes, as far as its tools and envelopes reach: the JSON parser's error
// messages, lax coercion of tool arguments, and ValidationError's text. Agents read these
// strings, so they are copied as pydantic writes them.
// Affinity: functions; any thread.
namespace fv::agentapi::detail {

// repr() of the value json.loads would give: None, True, 5, 1.5, 'text', [..], {'k': v}.
std::string py_repr(const Json& value);
// type(value).__name__: NoneType, bool, int, float, str, list, dict.
std::string_view py_type_name(const Json& value);
// True when a string holds a lone surrogate (its three-byte encoding, as json.loads of "\ud83d"
// gives it): pydantic refuses such text as "unable to parse raw data as a unicode string".
bool has_surrogate(std::string_view utf8);

// One line error of a ValidationError.
struct LineError {
    std::string loc;  // "limit", "types.0", "changes.auto_paste.bool", "JSONRPCRequest.id"
    std::string type; // "less_than_equal"
    std::string message;
    Json input;
};

// str(ValidationError): "<n> validation error(s) for <title>", then per error its location,
// "  <message> [type=<type>, input_value=<repr, cut to 25 + '...' + 24 code points past 50>,
// input_type=<name>]" and "    For further information visit
// https://errors.pydantic.dev/2.13/v/<type>", lines joined with "\n".
std::string format_validation_error(std::string_view title, const std::vector<LineError>& errors);

// pydantic_core.from_json(body): JSON with NaN, Infinity and -Infinity, a duplicate key keeping
// its last value. A failure is jiter's "<message> at line L column C" ("EOF while parsing an
// object at line 1 column 1", "trailing comma at line 1 column 48", "lone leading surrogate in
// hex escape ..."). Integers beyond 64 bits become floats (Python keeps them exact).
std::expected<Json, std::string> from_json(std::string_view body);

// Lax coercions of pydantic's validators. Each gives the value or the error (type, message);
// the caller adds the location and the input.
struct Refusal {
    std::string type;
    std::string message;
};
// int: a JSON integer; a bool as 0 or 1; a float without a fraction; a str of an optionally
// signed integer (surrounding whitespace, '_' between digits and a ".0" fraction allowed).
std::expected<std::int64_t, Refusal> lax_int(const Json& value);
// float: a number or bool, a str float() reads (surrounding whitespace allowed); never inf or nan
// (allow_inf_nan=False).
std::expected<double, Refusal> lax_float(const Json& value);
// bool: a bool; 0 or 1 (int or float); a str that is one of 0 off f false n no 1 on t true y yes
// in any case.
std::expected<bool, Refusal> lax_bool(const Json& value);
// str: only a str, and only valid Unicode.
std::expected<std::string, Refusal> lax_str(const Json& value);
// datetime: a str in RFC 3339 form (T, t, _ or space between date and time, seconds and fraction
// optional, Z or an offset) or a date alone (midnight), or a number or numeric str of seconds
// since 1970 (milliseconds above 2e10), which is UTC. Errors are speedate's date errors ("input is
// too short", "month value is outside expected range of 1-12", "unexpected extra characters at
// the end of the input", ...).
std::expected<py::DateTime, Refusal> lax_datetime(const Json& value);

} // namespace fv::agentapi::detail
