// The v16 and v17 migrations (tests/test_personalize_foundation_history.py,
// tests/test_stats_features.py), and DatabaseManager's start-up rules that no OpenWhisper test
// pins one by one: the version row, the FTS index and the delivery trigger, the fv_* version
// table, a failed migration, the JSON import's edge cases, readers and close.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <variant>

#include "db/database.h"
#include "db/schema.h"
#include "db_test_support.h"
#include "sql/db.h"
#include "testkit/testkit.h"

using fv::Database;
using fv::db_test::legacy_db;
using fv::db_test::names;
using fv::db_test::open_and_close;
using fv::db_test::schema_version_row;
using fv::sql::Connection;
using fv::sql::OpenMode;

namespace {

std::int64_t integer(Connection& conn, std::string_view sql) {
    return std::get<std::int64_t>(conn.scalar(sql));
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
}

void set_version(const std::filesystem::path& path, int version) {
    Connection conn = Connection::open(path, OpenMode::ReadWrite);
    conn.exec("UPDATE schema_version SET version = " + std::to_string(version));
}

} // namespace

// from
// tests/test_personalize_foundation_history.py::test_v16_database_gains_the_context_columns_and_stats_table
TEST_CASE("test_v16_database_gains_the_context_columns_and_stats_table",
          "[test_personalize_foundation_history]") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "legacy_v16.db";
    std::filesystem::copy_file(fv::db_test::fixture("schema_v16.db"), path);

    REQUIRE(fv::schema::schema_version == 18);
    {
        Database database = Database::open_or_create(path);
        database.read([](Connection& conn) {
            const auto columns = names(conn, "PRAGMA table_info(transcription_history)", 1);
            for (const auto column : fv::schema::transcription_history::context_columns) {
                CHECK(columns.contains(std::string(column)));
            }
            CHECK(schema_version_row(conn) == fv::schema::schema_version);
            CHECK(names(conn, "SELECT name FROM sqlite_master WHERE type='table'", 0)
                      .contains("dictation_stats"));

            auto legacy = conn.prepare("SELECT text, app_id, app_name, app_category, "
                                       "cleanup_level, entry_kind, cleaned_text, language "
                                       "FROM transcription_history WHERE id = 'old'");
            REQUIRE(legacy.step());
            CHECK(std::get<std::string>(legacy.column(0)) == "kept text");
            // Legacy rows derive their kind from source_name.
            for (int i = 1; i < legacy.column_count(); ++i) {
                CHECK(std::holds_alternative<std::nullptr_t>(legacy.column(i)));
            }
        });
        database.close();
    }

    // Running the migration again finds every column already there.
    set_version(path, 16);
    Database database = Database::open_or_create(path);
    CHECK(database.read([](Connection& conn) {
        return std::get<std::string>(
            conn.scalar("SELECT text FROM transcription_history WHERE id = 'old'"));
    }) == "kept text");
    database.close();
}

// from tests/test_stats_features.py::test_a_v17_stats_table_gains_the_feature_columns
TEST_CASE("test_a_v17_stats_table_gains_the_feature_columns", "[test_stats_features]") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "v17.db";
    {
        Connection conn = Connection::open(path, OpenMode::Create);
        auto tx = conn.begin();
        conn.exec("CREATE TABLE schema_version (version INTEGER PRIMARY KEY)");
        conn.exec("INSERT INTO schema_version(version) VALUES (17)");
        conn.exec(
            "CREATE TABLE dictation_stats (id INTEGER PRIMARY KEY AUTOINCREMENT, entry_id TEXT, "
            "timestamp TEXT NOT NULL, entry_kind TEXT NOT NULL, app_name TEXT, app_category TEXT, "
            "cleanup_level TEXT, spoken_words INTEGER NOT NULL, final_words INTEGER NOT NULL, "
            "words_edited INTEGER NOT NULL, audio_seconds REAL, language TEXT)");
        conn.exec("INSERT INTO dictation_stats (timestamp, entry_kind, spoken_words, final_words, "
                  "words_edited) VALUES ('2026-10-05T10:00:00', 'dictation', 4, 4, 0)");
        tx.commit();
    }
    for (int opening = 0; opening < 2; ++opening) {
        Database database = Database::open_or_create(path);
        database.read([](Connection& conn) {
            auto row = conn.prepare("SELECT target, snippets, snippet_words, corrections, shots, "
                                    "spoken_words FROM dictation_stats");
            REQUIRE(row.step());
            CHECK(std::holds_alternative<std::nullptr_t>(row.column(0)));
            for (int i = 1; i <= 4; ++i) {
                CHECK(std::get<std::int64_t>(row.column(i)) == 0);
            }
            CHECK(std::get<std::int64_t>(row.column(5)) == 4);
            CHECK_FALSE(row.step());
            CHECK(schema_version_row(conn) == fv::schema::schema_version);
        });
        database.close();
        set_version(path, 17);
    }
}

TEST_CASE("the schema_version row says 18 whatever the file held") {
    const fv::testkit::TempDataRoot root;

    SECTION("a newer file is brought back to 18 without migrating") {
        const auto path = legacy_db(root.path(), 99, {});
        open_and_close(path);
        Connection conn = Connection::open(path, OpenMode::ReadOnly);
        CHECK(integer(conn, "SELECT count(*) FROM schema_version") == 1);
        CHECK(schema_version_row(conn) == 18);
    }

    SECTION("an empty schema_version table runs no migration and gains the row") {
        const auto path = root.path() / "empty_version.db";
        {
            Connection conn = Connection::open(path, OpenMode::Create);
            conn.exec("CREATE TABLE schema_version (version INTEGER PRIMARY KEY)");
            // Without migrations the v7 table keeps its columns: create_all leaves it alone.
            conn.exec("CREATE TABLE transcription_history (id TEXT PRIMARY KEY, "
                      "text TEXT NOT NULL, timestamp TEXT NOT NULL, model TEXT NOT NULL)");
        }
        open_and_close(path);
        Connection conn = Connection::open(path, OpenMode::ReadOnly);
        CHECK(schema_version_row(conn) == 18);
        CHECK_FALSE(
            names(conn, "PRAGMA table_info(transcription_history)", 1).contains("raw_text"));
        // create_all makes no index for a table that already exists.
        CHECK(integer(conn, "SELECT count(*) FROM sqlite_master WHERE name = "
                            "'idx_history_timestamp'") == 0);
    }
}

TEST_CASE("a failed migration leaves the file as it was") {
    const fv::testkit::TempDataRoot root;
    // PRAGMA table_info reports "TARGET", so v18 adds "target", which SQLite refuses as a
    // duplicate column (names are case-insensitive), as OpenWhisper's migration would.
    const auto path =
        legacy_db(root.path(), 17,
                  {"CREATE TABLE dictation_stats (id INTEGER PRIMARY KEY, TARGET TEXT)",
                   "CREATE TABLE transcription_history (id TEXT PRIMARY KEY, text TEXT NOT NULL, "
                   "timestamp TEXT NOT NULL, model TEXT NOT NULL)"});

    CHECK_THROWS_WITH(Database::open_or_create(path),
                      Catch::Matchers::ContainsSubstring("duplicate column name"));

    Connection conn = Connection::open(path, OpenMode::ReadOnly);
    CHECK(schema_version_row(conn) == 17);
    CHECK_FALSE(names(conn, "PRAGMA table_info(transcription_history)", 1).contains("app_id"));
    CHECK(integer(conn, "SELECT count(*) FROM sqlite_master WHERE name = 'meeting_sessions'") == 0);
}

TEST_CASE("the meeting FTS index and delivery trigger follow meeting_segments") {
    const fv::testkit::TempDataRoot root;
    Database database = Database::open_or_create(root.path() / "meetings.db");
    database.write([](Connection& conn) {
        auto tx = conn.begin();
        conn.exec("INSERT INTO meeting_sessions (id, title, status, started_at, paused_total_s, "
                  "host_token, guest_token, cloud_enabled, spool_dir, state_seq) VALUES "
                  "('m', 't', 'active', '2026-11-14T09:00:00+00:00', 0.0, 'h', 'g', 0, '', 0)");
        for (const auto* id : {"s2", "s1"}) {
            auto insert = conn.prepare(
                "INSERT INTO meeting_segments (id, meeting_id, channel, start_s, end_s, text, "
                "speaker_source, speaker_pinned, created_at) VALUES (?, 'm', 'mic', 0, 1, "
                "'budget review tomorrow', 'channel', 0, '2026-11-14T09:00:00+00:00')");
            insert.bind(1, std::string(id));
            insert.step();
        }
        tx.commit();
        CHECK(integer(conn, "SELECT count(*) FROM meeting_segments_fts WHERE "
                            "meeting_segments_fts MATCH 'budget'") == 2);
        CHECK(names(conn, "SELECT segment_id || ':' || seq FROM meeting_segment_delivery", 0) ==
              std::set<std::string>{"s2:1", "s1:2"});

        conn.exec("UPDATE meeting_segments SET text = 'lunch plans' WHERE id = 's1'");
        CHECK(integer(conn, "SELECT count(*) FROM meeting_segments_fts WHERE "
                            "meeting_segments_fts MATCH 'budget'") == 1);
        // ON DELETE CASCADE needs foreign_keys=ON on the connection.
        conn.exec("DELETE FROM meeting_sessions WHERE id = 'm'");
        CHECK(integer(conn, "SELECT count(*) FROM meeting_segment_delivery") == 0);
        CHECK(integer(conn, "SELECT count(*) FROM meeting_segments_fts WHERE "
                            "meeting_segments_fts MATCH 'lunch'") == 0);
    });
    database.close();
}

TEST_CASE("connections run in WAL with OpenWhisper's pragmas, readers are query-only") {
    const fv::testkit::TempDataRoot root;
    Database database = Database::open_or_create(root.path() / "pragmas.db");
    database.write([](Connection& conn) {
        CHECK(std::get<std::string>(conn.scalar("PRAGMA journal_mode")) == "wal");
        CHECK(integer(conn, "PRAGMA foreign_keys") == 1);
        CHECK(integer(conn, "PRAGMA synchronous") == 1); // NORMAL
        CHECK(integer(conn, "PRAGMA busy_timeout") == 30000);
    });
    // A nested write on the same thread reuses the writer instead of deadlocking.
    database.write([&](Connection& outer) {
        database.write([&](Connection& inner) { CHECK(&outer == &inner); });
    });
    database.read([](Connection& conn) {
        CHECK(integer(conn, "PRAGMA query_only") == 1);
        CHECK(integer(conn, "PRAGMA foreign_keys") == 1);
        CHECK_THROWS_AS(conn.exec("DELETE FROM schema_version"), fv::sql::Error);
    });
    Connection* first = nullptr;
    database.read([&](Connection& conn) { first = &conn; });
    database.read([&](Connection& conn) { CHECK(&conn == first); });

    SECTION("fv_schema_version holds OpenWhisper_CPP's own version") {
        CHECK(database.read([](Connection& conn) {
            return integer(conn, "SELECT version FROM fv_schema_version");
        }) == fv::schema::fv_schema_version);
    }

    SECTION("a closed database refuses reads and writes") {
        database.close();
        database.close();
        CHECK_THROWS_WITH(database.write([](Connection&) {}), "The database is closed.");
        CHECK_THROWS_WITH(database.read([](Connection&) {}), "The database is closed.");
        CHECK(database.schema_version() == 18);
    }

    SECTION("a moved database keeps working, the moved-from one refuses") {
        Database moved = std::move(database);
        CHECK(moved.read([](Connection& conn) { return schema_version_row(conn); }) == 18);
        CHECK_THROWS_AS(database.write([](Connection&) {}), fv::sql::Error);
        moved.close();
    }
}

TEST_CASE("readers are pooled, not kept per thread") {
    const fv::testkit::TempDataRoot root;
    Database database = Database::open_or_create(root.path() / "readers.db");

    SECTION("a thread that has ended hands its reader to the next thread") {
        // Threads come and go (pools expire idle ones): a reader per thread id would pile up a
        // connection for every thread that ever read.
        const Connection* first = nullptr;
        const Connection* second = nullptr;
        std::thread([&] { database.read([&](Connection& conn) { first = &conn; }); }).join();
        std::thread([&] { database.read([&](Connection& conn) { second = &conn; }); }).join();
        CHECK(first != nullptr);
        CHECK(first == second);
    }

    SECTION("readers in use at the same time are different connections") {
        database.read([&](Connection& outer) {
            database.read([&](Connection& inner) { CHECK(&outer != &inner); });
        });
    }

    SECTION("a reader left inside a transaction is not handed out again") {
        const Connection* leaked = nullptr;
        database.read([&](Connection& conn) {
            conn.exec("BEGIN");
            leaked = &conn;
        });
        database.read([&](Connection& conn) {
            CHECK(&conn != leaked);
            CHECK(conn.autocommit());
        });
    }

    SECTION("close() leaves a read in progress to finish on its own reader") {
        std::promise<void> reading;
        std::promise<void> closed;
        std::int64_t version = 0;
        std::thread reader([&] {
            database.read([&](Connection& conn) {
                reading.set_value();
                closed.get_future().wait();
                version = schema_version_row(conn);
            });
        });
        reading.get_future().wait();
        database.close();
        closed.set_value();
        reader.join();
        CHECK(version == fv::schema::schema_version);
        CHECK_THROWS_WITH(database.read([](Connection&) {}), "The database is closed.");
    }
    database.close();
}

TEST_CASE("a schema_version that is not a number fails the start-up with sql::Error") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "text_version.db";
    {
        Connection conn = Connection::open(path, OpenMode::Create);
        conn.exec("CREATE TABLE schema_version (version TEXT)");
        conn.exec("INSERT INTO schema_version VALUES ('seventeen')");
    }
    // Python: `current_version < SCHEMA_VERSION` raises TypeError, and nothing is created.
    CHECK_THROWS_AS(Database::open_or_create(path), fv::sql::Error);
    Connection conn = Connection::open(path, OpenMode::ReadOnly);
    CHECK(integer(conn, "SELECT count(*) FROM sqlite_master WHERE name = 'meeting_sessions'") == 0);
}

TEST_CASE("the JSON history import runs once, only into an empty history") {
    const fv::testkit::TempDataRoot root;
    const auto json_path = root.path() / "transcription_history.json";
    const auto db_path = root.path() / "test.db";
    auto history_rows = [&] {
        Connection conn = Connection::open(db_path, OpenMode::ReadOnly);
        return integer(conn, "SELECT count(*) FROM transcription_history");
    };

    SECTION("missing keys take the model's fallbacks, a repeated id is merged") {
        write_text(json_path, R"({"entries": [{"id": "a"}, {"id": "b", "text": "one"},
                                              {"id": "b", "text": "two", "file_size": 5}]})");
        open_and_close(db_path);
        Connection conn = Connection::open(db_path, OpenMode::ReadOnly);
        auto rows = conn.prepare("SELECT id, text, timestamp, model, file_size "
                                 "FROM transcription_history ORDER BY id");
        REQUIRE(rows.step());
        CHECK(std::get<std::string>(rows.column(1)).empty());
        CHECK(std::get<std::string>(rows.column(2)).empty());
        CHECK(std::get<std::string>(rows.column(3)).empty());
        REQUIRE(rows.step());
        CHECK(std::get<std::string>(rows.column(1)) == "two");
        CHECK(std::get<std::int64_t>(rows.column(4)) == 5);
        CHECK_FALSE(rows.step());
        CHECK(std::filesystem::exists(root.path() / "transcription_history.json.bak"));
    }

    SECTION("no entries: nothing imported and the file stays") {
        write_text(json_path, R"({"entries": []})");
        open_and_close(db_path);
        CHECK(history_rows() == 0);
        CHECK(std::filesystem::exists(json_path));
    }

    SECTION("an entry that breaks a constraint imports nothing and keeps the file") {
        write_text(json_path, R"({"entries": [{"id": "a", "text": "ok"}, {"id": "b",
                                               "text": null}]})");
        CHECK_NOTHROW(open_and_close(db_path));
        CHECK(history_rows() == 0);
        CHECK(std::filesystem::exists(json_path));
    }

    SECTION("a file that is not JSON is logged and kept") {
        write_text(json_path, "{not json");
        CHECK_NOTHROW(open_and_close(db_path));
        CHECK(history_rows() == 0);
        CHECK(std::filesystem::exists(json_path));
    }

    SECTION("a history that already has entries is not imported into") {
        open_and_close(db_path);
        {
            Connection conn = Connection::open(db_path, OpenMode::ReadWrite);
            conn.exec("INSERT INTO transcription_history (id, text, timestamp, model) VALUES "
                      "('kept', 'x', '2026-11-14T00:00:00+00:00', 'parakeet')");
        }
        write_text(json_path, R"({"entries": [{"id": "a", "text": "new"}]})");
        open_and_close(db_path);
        CHECK(history_rows() == 1);
        CHECK(std::filesystem::exists(json_path));
    }

#ifdef _WIN32
    SECTION("an existing .bak keeps the JSON file in place after the import, as os.rename") {
        write_text(json_path, R"({"entries": [{"id": "a", "text": "new"}]})");
        write_text(root.path() / "transcription_history.json.bak", "older");
        open_and_close(db_path);
        CHECK(history_rows() == 1);
        CHECK(std::filesystem::exists(json_path));
    }
#endif
}
