#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include "base/json.h"
#include "db/database.h"
#include "sql/db.h"

namespace fv::db_test {

// tests/fixtures/db: OpenWhisper-made databases (tools/parity/cases/services.database.py).
std::filesystem::path fixture(std::string_view name);

// The database file a test made, copied from a fixture into `folder`.
std::filesystem::path copy_fixture(std::string_view name, const std::filesystem::path& folder);

// tests/test_database.py _legacy_db: a schema_version row at `version` plus `statements`.
std::filesystem::path legacy_db(const std::filesystem::path& folder, int version,
                                std::initializer_list<std::string_view> statements);

// Opens with fv::Database and closes again (DatabaseManager(db_path=...).close()).
void open_and_close(const std::filesystem::path& path);

// Column `column` of every row of `query`, as text.
std::set<std::string> names(sql::Connection&, std::string_view query, int column);
std::int64_t schema_version_row(sql::Connection&);

// A SQL value as JSON: integers, floats, text, null; a blob as {"$hex": "..."}.
Json to_json(const sql::Value&);

// What services.database.py's _dump records: sqlite_master (type, name, tbl_name, sql) ORDER
// BY type, name; PRAGMA table_info of every table; every row of every ordinary table ORDER BY
// rowid; schema_version. FioriVoce's fv_* tables are left out, as OpenWhisper has none.
Json dump(const std::filesystem::path& path);

// Runs of whitespace as one space, none at either end: the golden comparator for stored SQL.
std::string normalize_sql(std::string_view sql);

} // namespace fv::db_test
