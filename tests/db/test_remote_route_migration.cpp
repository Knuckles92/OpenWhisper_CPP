// The v13 migration (tests/test_meeting_remote.py): a database from before meetings could be
// transcribed on a paired computer gains the route column, and its meetings stay local ones.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <variant>

#include "db/database.h"
#include "db/schema.h"
#include "db_test_support.h"
#include "sql/db.h"
#include "testkit/testkit.h"

using fv::Database;
using fv::sql::Connection;
using fv::sql::OpenMode;

TEST_CASE("test_v12_migration_preserves_local_meetings_and_adds_remote_route",
          "[test_meeting_remote]") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "migration.db";
    fv::db_test::open_and_close(path);
    {
        Connection conn = Connection::open(path, OpenMode::ReadWrite);
        conn.exec("INSERT INTO meeting_sessions (id, title, status, started_at, paused_total_s, "
                  "host_token, guest_token, cloud_enabled, asr_model, spool_dir, state_seq) VALUES "
                  "('m_remote', 'Remote meeting', 'active', '2026-09-27T12:00:00Z', 0, 'host', "
                  "'guest', 0, 'base', 'spool', 0)");
        conn.exec("ALTER TABLE meeting_sessions DROP COLUMN asr_remote_json");
        conn.exec("UPDATE schema_version SET version = 12");
        CHECK_FALSE(fv::db_test::names(conn, "PRAGMA table_info(meeting_sessions)", 1)
                        .contains(std::string(fv::schema::meeting_sessions::asr_remote_json)));
    }

    Database upgraded = Database::open_or_create(path);
    upgraded.read([](Connection& conn) {
        CHECK(fv::db_test::schema_version_row(conn) == fv::schema::schema_version);
        auto row = conn.prepare(
            "SELECT title, asr_model, asr_remote_json FROM meeting_sessions WHERE id = 'm_remote'");
        REQUIRE(row.step());
        CHECK(std::get<std::string>(row.column(0)) == "Remote meeting");
        CHECK(std::get<std::string>(row.column(1)) == "base");
        // No route: the meeting was, and stays, a local one.
        CHECK(std::holds_alternative<std::nullptr_t>(row.column(2)));
    });
    upgraded.close();
}
