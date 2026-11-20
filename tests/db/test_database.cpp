// tests/test_database.py: schema creation, the migrations, the JSON import and concurrent first
// opens (the CRUD and search tests are HistoryRepository's, tests/history). Every test opens its
// files under a testkit data root.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "db/database.h"
#include "db/schema.h"
#include "db_test_support.h"
#include "sql/db.h"
#include "testkit/testkit.h"

using fv::Database;
using fv::db_test::legacy_db;
using fv::db_test::names;
using fv::db_test::schema_version_row;

namespace {

constexpr std::string_view history_columns = "PRAGMA table_info(transcription_history)";

struct Migrated {
    std::set<std::string> names;
    std::int64_t version = 0;
};

// _migrated: opens with the current build; `query`'s names and the schema version.
Migrated migrated(const std::filesystem::path& path, std::string_view query, int column) {
    Database database = Database::open_or_create(path);
    Migrated result = database.read([&](fv::sql::Connection& conn) {
        return Migrated{names(conn, query, column), schema_version_row(conn)};
    });
    database.close();
    return result;
}

std::set<std::string> table_names(Database& database) {
    return database.read([](fv::sql::Connection& conn) {
        return names(conn,
                     "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE "
                     "'sqlite\\_%' ESCAPE '\\'",
                     0);
    });
}

} // namespace

// from tests/test_database.py::TestDatabaseManager::test_schema_creation
TEST_CASE("test_schema_creation", "[test_database]") {
    const fv::testkit::TempDataRoot root;
    Database database = Database::open_or_create(root.path() / "test.db");
    const auto tables = table_names(database);

    CHECK(tables.contains("schema_version"));
    CHECK(tables.contains("transcription_history"));
    CHECK_FALSE(tables.contains("meetings"));
    CHECK_FALSE(tables.contains("meeting_chunks"));
    CHECK_FALSE(tables.contains("meeting_insights"));
    CHECK(database.schema_version() == 18);
    CHECK(database.schema_version() == fv::schema::schema_version);
}

// from tests/test_database.py::TestDatabaseManager::test_migration_removes_meeting_tables
TEST_CASE("test_migration_removes_meeting_tables", "[test_database]") {
    const fv::testkit::TempDataRoot root;
    const auto path =
        legacy_db(root.path(), 5,
                  {R"(
            CREATE TABLE meetings (
                id TEXT PRIMARY KEY,
                title TEXT,
                start_time TEXT
            )
            )",
                   R"(
            CREATE TABLE meeting_chunks (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                meeting_id TEXT NOT NULL,
                chunk_index INTEGER NOT NULL,
                text TEXT NOT NULL,
                timestamp TEXT NOT NULL
            )
            )",
                   R"(
            CREATE TABLE meeting_insights (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                meeting_id TEXT NOT NULL,
                insight_type TEXT NOT NULL,
                content TEXT NOT NULL,
                custom_prompt TEXT,
                generated_at TEXT NOT NULL
            )
            )",
                   "CREATE INDEX idx_insights_meeting_id ON meeting_insights(meeting_id)",
                   "CREATE UNIQUE INDEX idx_insights_unique "
                   "ON meeting_insights(meeting_id, insight_type, COALESCE(custom_prompt, ''))",
                   "CREATE INDEX idx_meetings_start_time ON meetings(start_time)",
                   "CREATE INDEX idx_chunks_meeting_id ON meeting_chunks(meeting_id)"});

    const auto [tables, version] =
        migrated(path, "SELECT name FROM sqlite_master WHERE type='table'", 0);

    // Legacy names must remain absent (startup drop + never recreated).
    CHECK_FALSE(tables.contains("meeting_insights"));
    CHECK_FALSE(tables.contains("meeting_chunks"));
    CHECK_FALSE(tables.contains("meetings"));
    CHECK(tables.contains("meeting_sessions"));
    CHECK(version == fv::schema::schema_version);
}

// from tests/test_database.py::TestDatabaseManager::test_migration_adds_raw_text_column
TEST_CASE("test_migration_adds_raw_text_column", "[test_database]") {
    const fv::testkit::TempDataRoot root;
    const auto path = legacy_db(root.path(), 7, {R"(
            CREATE TABLE transcription_history (
                id TEXT PRIMARY KEY,
                text TEXT NOT NULL,
                timestamp TEXT NOT NULL,
                model TEXT NOT NULL,
                audio_file TEXT,
                transcription_time REAL,
                audio_duration REAL,
                file_size INTEGER
            )
            )"});

    const auto [columns, version] = migrated(path, history_columns, 1);

    CHECK(columns.contains("raw_text"));
    CHECK(columns.contains("cleanup_provider"));
    CHECK(columns.contains("cleanup_model"));
    CHECK(columns.contains("source_name"));
    CHECK(version == fv::schema::schema_version);
}

// from tests/test_database.py::TestDatabaseManager::test_migration_adds_cleanup_columns
TEST_CASE("test_migration_adds_cleanup_columns", "[test_database]") {
    const fv::testkit::TempDataRoot root;
    const auto path = legacy_db(root.path(), 8, {R"(
            CREATE TABLE transcription_history (
                id TEXT PRIMARY KEY,
                text TEXT NOT NULL,
                raw_text TEXT,
                timestamp TEXT NOT NULL,
                model TEXT NOT NULL,
                audio_file TEXT,
                transcription_time REAL,
                audio_duration REAL,
                file_size INTEGER
            )
            )"});

    const auto [columns, version] = migrated(path, history_columns, 1);

    CHECK(columns.contains("cleanup_provider"));
    CHECK(columns.contains("cleanup_model"));
    CHECK(columns.contains("source_name"));
    CHECK(version == fv::schema::schema_version);
}

// from tests/test_database.py::TestDatabaseManager::test_migration_adds_source_name_column
TEST_CASE("test_migration_adds_source_name_column", "[test_database]") {
    const fv::testkit::TempDataRoot root;
    const auto path = legacy_db(root.path(), 9, {R"(
            CREATE TABLE transcription_history (
                id TEXT PRIMARY KEY,
                text TEXT NOT NULL,
                raw_text TEXT,
                timestamp TEXT NOT NULL,
                model TEXT NOT NULL,
                audio_file TEXT,
                transcription_time REAL,
                audio_duration REAL,
                file_size INTEGER,
                cleanup_provider TEXT,
                cleanup_model TEXT
            )
            )"});

    const auto [columns, version] = migrated(path, history_columns, 1);

    CHECK(columns.contains("source_name"));
    CHECK(version == fv::schema::schema_version);
}

// from tests/test_database.py::TestDatabaseManager::test_migration_adds_agent_endpoint_json
TEST_CASE("test_migration_adds_agent_endpoint_json", "[test_database]") {
    const fv::testkit::TempDataRoot root;
    const auto path = legacy_db(root.path(), 10, {R"(
            CREATE TABLE meeting_sessions (
                id TEXT PRIMARY KEY,
                title TEXT NOT NULL DEFAULT '',
                status TEXT NOT NULL,
                started_at TEXT NOT NULL,
                ended_at TEXT,
                paused_total_s REAL NOT NULL DEFAULT 0,
                host_token TEXT NOT NULL,
                guest_token TEXT NOT NULL,
                cloud_enabled BOOLEAN NOT NULL DEFAULT 0,
                asr_model TEXT,
                agent_provider TEXT,
                agent_model TEXT,
                spool_dir TEXT NOT NULL,
                state_json TEXT,
                state_seq INTEGER NOT NULL DEFAULT 0,
                app_pid INTEGER,
                app_heartbeat_at TEXT
            )
            )"});

    const auto [columns, version] = migrated(path, "PRAGMA table_info(meeting_sessions)", 1);

    CHECK(columns.contains("agent_endpoint_json"));
    CHECK(version == fv::schema::schema_version);
}

// from tests/test_database.py::TestJsonMigration::test_history_migration
TEST_CASE("test_history_migration", "[test_database]") {
    const fv::testkit::TempDataRoot root;
    const auto json_path = root.path() / "transcription_history.json";
    {
        std::ofstream file(json_path, std::ios::binary);
        file << R"({"entries": [)"
             << R"({"id": "4f1c2a9e-0d55-4bb1-9f1a-1b6f2f6b0a01", "text": "Old transcription 1", )"
             << R"("timestamp": "2026-11-14T10:00:00.000001", "model": "local_whisper", )"
             << R"("audio_file": null, "transcription_time": 2.0, "audio_duration": 5.0, )"
             << R"("file_size": 80000}, )"
             << R"({"id": "4f1c2a9e-0d55-4bb1-9f1a-1b6f2f6b0a02", "text": "Old transcription 2", )"
             << R"("timestamp": "2026-11-14T10:00:01.000002", "model": "api_whisper", )"
             << R"("audio_file": "recording.wav", "transcription_time": 1.0, )"
             << R"("audio_duration": 3.0, "file_size": 48000}]})";
    }

    Database database = Database::open_or_create(root.path() / "test.db");
    const auto count = database.read([](fv::sql::Connection& conn) {
        return std::get<std::int64_t>(conn.scalar("SELECT count(*) FROM transcription_history"));
    });
    CHECK(count == 2);
    CHECK(std::filesystem::exists(root.path() / "transcription_history.json.bak"));
    CHECK_FALSE(std::filesystem::exists(json_path));
    database.close();
}

// from
// tests/test_database.py::TestLazyDatabaseManager::test_concurrent_first_access_creates_schema_once
TEST_CASE("test_concurrent_first_access_creates_schema_once", "[test_database]") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "race.db";
    std::mutex errors_mutex;
    std::vector<std::string> errors;
    auto record = [&](const std::exception& error) {
        const std::lock_guard lock(errors_mutex);
        errors.emplace_back(error.what());
    };

    SECTION("eight threads opening the same new file") {
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([&] {
                try {
                    Database database = Database::open_or_create(path);
                    const auto rows = database.read([](fv::sql::Connection& conn) {
                        return std::get<std::int64_t>(
                            conn.scalar("SELECT count(*) FROM schema_version"));
                    });
                    if (rows < 1) {
                        throw std::runtime_error("no schema_version row");
                    }
                } catch (const std::exception& error) {
                    record(error);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
    }

    SECTION("eight threads sharing one database") {
        Database database = Database::open_or_create(path);
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([&, i] {
                try {
                    database.write([&](fv::sql::Connection& conn) {
                        auto tx = conn.begin(fv::sql::TxMode::Immediate);
                        auto insert = conn.prepare(
                            "INSERT INTO transcription_history (id, text, timestamp, model) "
                            "VALUES (?, 'x', '2026-11-14T00:00:00+00:00', 'parakeet')");
                        insert.bind(1, "entry-" + std::to_string(i));
                        insert.step();
                        tx.commit();
                    });
                    const auto rows = database.read([](fv::sql::Connection& conn) {
                        return std::get<std::int64_t>(
                            conn.scalar("SELECT count(*) FROM schema_version"));
                    });
                    if (rows < 1) {
                        throw std::runtime_error("no schema_version row");
                    }
                } catch (const std::exception& error) {
                    record(error);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        CHECK(database.read([](fv::sql::Connection& conn) {
            return std::get<std::int64_t>(
                conn.scalar("SELECT count(*) FROM transcription_history"));
        }) == 8);
        database.close();
    }

    CHECK(errors.empty());
    for (const auto& error : errors) {
        UNSCOPED_INFO(error);
    }
    Database database = Database::open_or_create(path);
    CHECK(table_names(database).contains("schema_version"));
    CHECK(database.read([](fv::sql::Connection& conn) {
        return std::get<std::int64_t>(conn.scalar("SELECT count(*) FROM schema_version"));
    }) == 1);
    database.close();
}

// from tests/test_database.py::test_suite_keeps_the_database_out_of_the_checkout
TEST_CASE("test_suite_keeps_the_database_out_of_the_checkout", "[test_database]") {
    // Every test here opens its files under a testkit data root, which FIORIVOCE_DATA_DIR names
    // for the test's lifetime; the app's own database resolves into it too.
    const fv::testkit::TempDataRoot root;
    const auto paths = fv::Paths::resolve(fv::identity(), fv::system_env());
    const auto database_file = paths.user_file(fv::identity().database_file);

    const auto checkout = std::filesystem::weakly_canonical(fv::testkit::source_root());
    const auto resolved = std::filesystem::weakly_canonical(database_file);
    CHECK(resolved != std::filesystem::weakly_canonical(checkout / fv::identity().database_file));
    CHECK(resolved.parent_path() == std::filesystem::weakly_canonical(root.path()));
    const auto relative = resolved.lexically_relative(checkout);
    CHECK((relative.empty() || *relative.begin() == ".."));
    CHECK(paths.user_file("transcription_history.json").parent_path() == paths.data_root());
}
