#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "history/entry.h"
#include "sql/db.h"

// Reading SQLite values into the history value types.
namespace fv::history_detail {

// "SELECT <every column in table order> FROM transcription_history".
const std::string& select_columns();
// The current row of a statement made from select_columns().
HistoryEntry read_entry(const sql::Statement& statement);

// A TEXT value, or nullopt for NULL. Numbers read as Python's str() of them would not appear in
// a TEXT column OpenWhisper wrote, so they are taken as text the way SQLite casts them.
std::optional<std::string> optional_text(const sql::Value& value);
// A REAL column: INTEGER values (SQLite stores whole numbers so) widen.
std::optional<double> optional_real(const sql::Value& value);
// An INTEGER column: a REAL value truncates.
std::optional<std::int64_t> optional_integer(const sql::Value& value);

} // namespace fv::history_detail
