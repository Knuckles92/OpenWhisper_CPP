#include <array>
#include <chrono>
#include <exception>
#include <format>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

#include "base/log.h"
#include "db/detail/startup.h"
#include "db/schema.h"
#include "pyport/number.h"
#include "sql/db.h"

namespace fv::db_detail {

namespace {

const log::Logger logger{"services.database"};

// create_engine(..., connect_args={"timeout": 30}).
constexpr std::chrono::milliseconds busy_timeout{30'000};
// SQLite result codes this file tests for (sqlite3.h stays out of fv_db).
constexpr int sqlite_busy = 5;
constexpr int sqlite_mismatch = 20;

using Columns = std::set<std::string, std::less<>>;

// _table_exists: a table of that name in sqlite_master.
bool table_exists(sql::Connection& conn, std::string_view table) {
    sql::Statement statement =
        conn.prepare("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
    statement.bind(1, std::string(table));
    return statement.step();
}

// _table_columns: column names; empty when the table does not exist.
Columns table_columns(sql::Connection& conn, std::string_view table) {
    Columns columns;
    sql::Statement statement = conn.prepare(std::format("PRAGMA table_info({})", table));
    while (statement.step()) {
        columns.insert(std::get<std::string>(statement.column(1)));
    }
    return columns;
}

// SQLAlchemy's has_table on SQLite: PRAGMA table_info finds a table (or view) of that name.
bool has_table(sql::Connection& conn, std::string_view table) {
    return !table_columns(conn, table).empty();
}

void add_text_columns(sql::Connection& conn, std::string_view table,
                      std::span<const std::string_view> columns, const Columns& existing) {
    for (const std::string_view column : columns) {
        if (!existing.contains(column)) {
            conn.exec(std::format("ALTER TABLE {} ADD COLUMN {} TEXT", table, column));
        }
    }
}

void add_text_columns_if_table_exists(sql::Connection& conn, std::string_view table,
                                      std::span<const std::string_view> columns) {
    if (table_exists(conn, table)) {
        add_text_columns(conn, table, columns, table_columns(conn, table));
    }
}

void add_text_columns_if_columns_found(sql::Connection& conn, std::string_view table,
                                       std::span<const std::string_view> columns) {
    const Columns existing = table_columns(conn, table);
    if (!existing.empty()) {
        add_text_columns(conn, table, columns, existing);
    }
}

// A step that logs "<label> failed: <error>" and rethrows, as the Python steps do.
template <class F> void logged_step(std::string_view label, F&& body) {
    try {
        body();
    } catch (const std::exception& error) {
        logger.error("Migration {} failed: {}", label, error.what());
        throw;
    }
}

void migrate_to_v6(sql::Connection& conn) {
    logged_step("v5->v6", [&] {
        conn.exec("DROP INDEX IF EXISTS idx_insights_unique");
        conn.exec("DROP INDEX IF EXISTS idx_insights_meeting_id");
        conn.exec("DROP TABLE IF EXISTS meeting_insights");
        logger.info("Migration v5->v6: Removed meeting_insights table");
    });
}

void migrate_to_v7(sql::Connection& conn) {
    logged_step("v6->v7", [&] {
        conn.exec("DROP INDEX IF EXISTS idx_chunks_meeting_id");
        conn.exec("DROP INDEX IF EXISTS idx_meetings_start_time");
        conn.exec("DROP INDEX IF EXISTS idx_insights_unique");
        conn.exec("DROP INDEX IF EXISTS idx_insights_meeting_id");
        conn.exec("DROP TABLE IF EXISTS meeting_insights");
        conn.exec("DROP TABLE IF EXISTS meeting_chunks");
        conn.exec("DROP TABLE IF EXISTS meetings");
        logger.info("Migration v6->v7: Removed meeting mode tables");
    });
}

void migrate_to_v8(sql::Connection& conn) {
    logged_step("v7->v8", [&] {
        static constexpr std::array<std::string_view, 1> columns = {"raw_text"};
        add_text_columns_if_table_exists(conn, "transcription_history", columns);
        logger.info("Migration v7->v8: Added raw_text to transcription_history");
    });
}

void migrate_to_v9(sql::Connection& conn) {
    logged_step("v8->v9", [&] {
        static constexpr std::array<std::string_view, 2> columns = {"cleanup_provider",
                                                                    "cleanup_model"};
        add_text_columns_if_table_exists(conn, "transcription_history", columns);
        logger.info("Migration v8->v9: Added cleanup_provider/cleanup_model "
                    "to transcription_history");
    });
}

void migrate_to_v10(sql::Connection&) {
    // The Meeting Mode tables are new in v10 and made by create_all after the migrations.
    logger.info("Migration v9->v10: Meeting Mode tables added via create_all");
}

void migrate_to_v11(sql::Connection& conn) {
    logged_step("v10->v11", [&] {
        static constexpr std::array<std::string_view, 1> columns = {"agent_endpoint_json"};
        add_text_columns_if_table_exists(conn, "meeting_sessions", columns);
        logger.info("Migration v10->v11: Added agent_endpoint_json "
                    "to meeting_sessions");
    });
}

void migrate_to_v12(sql::Connection& conn) {
    logged_step("v11->v12", [&] {
        static constexpr std::array<std::string_view, 1> columns = {"source_name"};
        add_text_columns_if_table_exists(conn, "transcription_history", columns);
        logger.info("Migration v11->v12: Added source_name to transcription_history");
    });
}

void migrate_to_v13(sql::Connection& conn) {
    static constexpr std::array<std::string_view, 1> columns = {"asr_remote_json"};
    add_text_columns_if_columns_found(conn, "meeting_sessions", columns);
}

void migrate_to_v14(sql::Connection& conn) {
    // record_sync itself is new and made by create_all.
    static constexpr std::array<std::string_view, 2> columns = {"origin_device_id",
                                                                "origin_device_name"};
    for (const std::string_view table : {"transcription_history", "meeting_sessions"}) {
        add_text_columns_if_columns_found(conn, table, columns);
    }
}

void migrate_to_v15(sql::Connection& conn) {
    static constexpr std::array<std::string_view, 1> columns = {"title"};
    add_text_columns_if_columns_found(conn, "transcription_history", columns);
}

void migrate_to_v17(sql::Connection& conn) {
    // dictation_stats is new and made by create_all.
    add_text_columns_if_columns_found(conn, "transcription_history",
                                      schema::transcription_history::context_columns);
}

void migrate_to_v18(sql::Connection& conn) {
    const Columns existing = table_columns(conn, "dictation_stats");
    if (existing.empty()) {
        return;
    }
    static constexpr std::array<std::string_view, 1> text_columns = {"target"};
    add_text_columns(conn, "dictation_stats", text_columns, existing);
    for (const std::string_view column : {"snippets", "snippet_words", "corrections", "shots"}) {
        if (!existing.contains(column)) {
            conn.exec(std::format(
                "ALTER TABLE dictation_stats ADD COLUMN {} INTEGER NOT NULL DEFAULT 0", column));
        }
    }
}

struct Step {
    int version; // the schema version the step brings the database to
    void (*migrate)(sql::Connection&);
};

// _MIGRATIONS: a step runs when the database is older than its version, in this order. v16
// needed none.
constexpr std::array<Step, 12> migrations = {{
    {6, migrate_to_v6},
    {7, migrate_to_v7},
    {8, migrate_to_v8},
    {9, migrate_to_v9},
    {10, migrate_to_v10},
    {11, migrate_to_v11},
    {12, migrate_to_v12},
    {13, migrate_to_v13},
    {14, migrate_to_v14},
    {15, migrate_to_v15},
    {17, migrate_to_v17},
    {18, migrate_to_v18},
}};

// The stored version as Python compares it with an int: an INTEGER or a REAL.
double stored_version(const sql::Value& value, std::string& label) {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        label = std::to_string(*integer);
        return static_cast<double>(*integer);
    }
    if (const auto* real = std::get_if<double>(&value)) {
        label = py::float_repr(*real);
        return *real;
    }
    // Python's `current_version < SCHEMA_VERSION` raises TypeError there; open_or_create
    // documents sql::Error for every start-up failure.
    throw sql::Error(sqlite_mismatch,
                     "The schema_version table holds a version that is not a number.");
}

void run_migrations(sql::Connection& conn, double from_version, const std::string& label) {
    logger.info("Running database migrations from v{} to v{}", label, schema::schema_version);
    for (const Step& step : migrations) {
        if (from_version < step.version) {
            step.migrate(conn);
        }
    }
    sql::Statement update = conn.prepare("UPDATE schema_version SET version = :v");
    update.bind(":v", std::int64_t{schema::schema_version});
    update.step();
    logger.info("Database migrated to schema version {}", schema::schema_version);
}

// _maybe_run_migrations: nothing on a fresh file or an empty schema_version.
void maybe_run_migrations(sql::Connection& conn) {
    if (!has_table(conn, "schema_version")) {
        return;
    }
    sql::Statement select = conn.prepare("SELECT version FROM schema_version LIMIT 1");
    if (!select.step()) {
        return;
    }
    std::string label;
    const double current = stored_version(select.column(0), label);
    select.reset();
    if (current < schema::schema_version) {
        run_migrations(conn, current, label);
    }
}

// Base.metadata.create_all: each missing table with its indexes. An existing table is left as it
// is, missing indexes included.
void create_all(sql::Connection& conn) {
    for (const schema::TableDdl& table : schema::tables()) {
        if (has_table(conn, table.name)) {
            continue;
        }
        conn.exec(table.create);
        for (const std::string_view index : table.indexes) {
            conn.exec(index);
        }
    }
}

// Track removal target of OpenWhisper's comment: 2026-12-01.
void drop_removed_meeting_tables(sql::Connection& conn) {
    conn.exec("DROP INDEX IF EXISTS idx_chunks_meeting_id");
    conn.exec("DROP INDEX IF EXISTS idx_meetings_start_time");
    conn.exec("DROP INDEX IF EXISTS idx_insights_unique");
    conn.exec("DROP INDEX IF EXISTS idx_insights_meeting_id");
    conn.exec("DROP TABLE IF EXISTS meeting_insights");
    conn.exec("DROP TABLE IF EXISTS meeting_chunks");
    conn.exec("DROP TABLE IF EXISTS meetings");
}

// Without FTS5, meeting search degrades and nothing else breaks: a warning only.
void ensure_meeting_fts(sql::Connection& conn) {
    conn.exec("SAVEPOINT meeting_fts");
    try {
        for (const std::string_view statement : schema::meeting_fts_statements()) {
            conn.exec(statement);
        }
        conn.exec("RELEASE meeting_fts");
    } catch (const sql::Error& error) {
        conn.exec("ROLLBACK TO meeting_fts");
        conn.exec("RELEASE meeting_fts");
        logger.warning("Could not initialize meeting FTS index: {}", error.what());
    }
}

// Commit order for transcript delivery. The backfill runs only when the counts differ, so a
// normal start neither scans the segments nor spends AUTOINCREMENT values on ignored inserts.
void ensure_meeting_delivery(sql::Connection& conn) {
    conn.exec(schema::meeting_delivery_trigger());
    const sql::Value missing = conn.scalar(R"(
                SELECT (SELECT count(*) FROM meeting_segments)
                     - (SELECT count(*) FROM meeting_segment_delivery)
            )");
    const auto* count = std::get_if<std::int64_t>(&missing);
    if (count != nullptr && *count != 0) {
        conn.exec(R"(
                    INSERT INTO meeting_segment_delivery(segment_id, meeting_id)
                    SELECT s.id, s.meeting_id FROM meeting_segments AS s
                    WHERE NOT EXISTS (
                        SELECT 1 FROM meeting_segment_delivery AS d
                        WHERE d.segment_id = s.id
                    )
                    ORDER BY s.rowid
                )");
    }
}

// The version row is replaced (all rows deleted, 18 inserted) whenever no row says 18, which
// also brings a newer file's row back to 18, as OpenWhisper does.
void ensure_version_row(sql::Connection& conn) {
    sql::Statement get = conn.prepare("SELECT version FROM schema_version WHERE version = ?");
    get.bind(1, std::int64_t{schema::schema_version});
    if (get.step()) {
        return;
    }
    conn.exec("DELETE FROM schema_version");
    sql::Statement insert = conn.prepare("INSERT INTO schema_version (version) VALUES (?)");
    insert.bind(1, std::int64_t{schema::schema_version});
    insert.step();
}

// OpenWhisper_CPP's own tables. Version 1 is fv_schema_version alone; a package adding an fv_*
// table adds a step here and raises schema::fv_schema_version.
void ensure_fv_schema(sql::Connection& conn) {
    conn.exec("CREATE TABLE IF NOT EXISTS fv_schema_version (\n"
              "\tversion INTEGER NOT NULL, \n"
              "\tPRIMARY KEY (version)\n"
              ")");
    const sql::Value stored = conn.scalar("SELECT max(version) FROM fv_schema_version");
    if (std::holds_alternative<std::nullptr_t>(stored)) {
        sql::Statement insert = conn.prepare("INSERT INTO fv_schema_version (version) VALUES (?)");
        insert.bind(1, std::int64_t{schema::fv_schema_version});
        insert.step();
    }
}

} // namespace

void configure_connection(sql::Connection& conn, bool read_only) {
    conn.set_busy_timeout(busy_timeout);
    conn.exec("PRAGMA foreign_keys=ON");
    // Changing to WAL takes an exclusive lock without consulting the busy handler, so a first
    // open racing another one retries here for as long as the busy timeout would wait.
    const auto deadline = std::chrono::steady_clock::now() + busy_timeout;
    for (;;) {
        try {
            conn.exec("PRAGMA journal_mode=WAL");
            break;
        } catch (const sql::Error& error) {
            if (error.primary_code() != sqlite_busy ||
                std::chrono::steady_clock::now() >= deadline) {
                throw;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    conn.exec("PRAGMA synchronous=NORMAL");
    if (read_only) {
        conn.exec("PRAGMA query_only=ON");
    }
}

void init_database(sql::Connection& conn) {
    sql::Transaction transaction = conn.begin(sql::TxMode::Immediate);
    // For existing databases, migrations run before create_all so that their ALTER TABLE
    // statements add the columns the models expect.
    maybe_run_migrations(conn);
    create_all(conn);
    drop_removed_meeting_tables(conn);
    ensure_meeting_fts(conn);
    ensure_meeting_delivery(conn);
    ensure_version_row(conn);
    ensure_fv_schema(conn);
    transaction.commit();
    logger.info("Database schema initialized");
}

} // namespace fv::db_detail
