#include "db_test_support.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "testkit/testkit.h"

namespace fv::db_test {

std::filesystem::path fixture(std::string_view name) {
    return testkit::source_root() / "tests" / "fixtures" / "db" / std::filesystem::path(name);
}

std::filesystem::path copy_fixture(std::string_view name, const std::filesystem::path& folder) {
    const auto target = folder / (std::string(name) + ".db");
    std::filesystem::copy_file(fixture(std::string(name) + ".db"), target,
                               std::filesystem::copy_options::overwrite_existing);
    return target;
}

std::filesystem::path legacy_db(const std::filesystem::path& folder, int version,
                                std::initializer_list<std::string_view> statements) {
    const auto path = folder / ("legacy_v" + std::to_string(version) + ".db");
    sql::Connection conn = sql::Connection::open(path, sql::OpenMode::Create);
    auto tx = conn.begin();
    conn.exec("CREATE TABLE schema_version (version INTEGER PRIMARY KEY)");
    conn.exec("INSERT INTO schema_version(version) VALUES (" + std::to_string(version) + ")");
    for (const std::string_view statement : statements) {
        conn.exec(statement);
    }
    tx.commit();
    return path;
}

void open_and_close(const std::filesystem::path& path) {
    Database database = Database::open_or_create(path);
    database.close();
}

std::set<std::string> names(sql::Connection& conn, std::string_view query, int column) {
    std::set<std::string> result;
    sql::Statement statement = conn.prepare(query);
    while (statement.step()) {
        result.insert(std::get<std::string>(statement.column(column)));
    }
    return result;
}

std::int64_t schema_version_row(sql::Connection& conn) {
    return std::get<std::int64_t>(conn.scalar("SELECT version FROM schema_version"));
}

Json to_json(const sql::Value& value) {
    return std::visit(
        [](const auto& v) -> Json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                return nullptr;
            } else if constexpr (std::is_same_v<T, sql::Blob>) {
                static constexpr char hex[] = "0123456789abcdef";
                std::string text;
                for (const std::byte b : v) {
                    text += hex[std::to_integer<int>(b) >> 4];
                    text += hex[std::to_integer<int>(b) & 0x0f];
                }
                return Json{{"$hex", text}};
            } else {
                return v;
            }
        },
        value);
}

namespace {

Json rows_of(sql::Connection& conn, const std::string& query) {
    Json rows = Json::array();
    sql::Statement statement = conn.prepare(query);
    while (statement.step()) {
        Json row = Json::array();
        for (int i = 0; i < statement.column_count(); ++i) {
            row.push_back(to_json(statement.column(i)));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

bool openwhisper_cpp_only(const Json& name) {
    return name.is_string() && name.get<std::string>().starts_with("fv_");
}

} // namespace

Json dump(const std::filesystem::path& path) {
    sql::Connection conn = sql::Connection::open(path, sql::OpenMode::ReadWrite);
    Json master = Json::array();
    for (Json& row : rows_of(conn, "SELECT type, name, tbl_name, sql FROM sqlite_master "
                                   "ORDER BY type, name")) {
        if (!openwhisper_cpp_only(row[1]) && !openwhisper_cpp_only(row[2])) {
            master.push_back(std::move(row));
        }
    }
    Json info = Json::object();
    Json rows = Json::object();
    for (const Json& table :
         rows_of(conn, "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")) {
        const std::string name = table[0].get<std::string>();
        if (name.starts_with("fv_")) {
            continue;
        }
        info[name] = rows_of(conn, "PRAGMA table_info(\"" + name + "\")");
        if (!name.starts_with("meeting_segments_fts")) {
            rows[name] = rows_of(conn, "SELECT * FROM \"" + name + "\" ORDER BY rowid");
        }
    }
    return Json{{"sqlite_master", std::move(master)},
                {"table_info", std::move(info)},
                {"rows", std::move(rows)},
                {"schema_version", rows_of(conn, "SELECT version FROM schema_version")}};
}

std::string normalize_sql(std::string_view sql) {
    std::string out;
    bool space = false;
    for (const char c : sql) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = !out.empty();
            continue;
        }
        if (space) {
            out += ' ';
            space = false;
        }
        out += c;
    }
    return out;
}

} // namespace fv::db_test
