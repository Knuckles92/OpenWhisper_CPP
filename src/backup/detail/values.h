#pragma once

#include <string>
#include <string_view>

#include "base/json.h"
#include "sql/db.h"

// Python's view of the SQLite values backup.py reads and binds.
namespace fv::backup::detail::values {

// str(value) of what sqlite3 returns: "None", an int's digits, a float's repr, the text, or a
// bytes repr (b'...').
std::string str(const sql::Value& value);
// str(value or ""): "" for None, 0, 0.0, "" and b"".
std::string str_or_empty(const sql::Value& value);
// int(value): py::TypeError for None, py::ValueError for text that is no integer or a NaN,
// py::OverflowError for an infinity.
int to_int(const sql::Value& value);

// A JSON value bound as sqlite3 binds the Python value json.loads made: None, int (a bool as 0
// or 1), float, str. A list or dict cannot be bound (ProgrammingError "Error binding parameter
// <index>: type '<list|dict>' is not supported", raised as std::runtime_error).
sql::Value from_json(const Json& value, int index);

// How pathlib compares path text on this platform: lowered on Windows, as it is elsewhere.
std::string fold_case(std::string_view text);

} // namespace fv::backup::detail::values
