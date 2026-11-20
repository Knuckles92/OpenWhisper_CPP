// OpenMode::Immutable (sweep I2): reading another app's database without leaving anything
// beside it, which a read-only open of a WAL database does (-wal, -shm).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <variant>

#include "sql/db.h"
#include "testkit/testkit.h"

using fv::sql::Connection;
using fv::sql::Error;
using fv::sql::OpenMode;
namespace fs = std::filesystem;

TEST_CASE("an immutable open of a WAL database reads it and creates nothing beside it") {
    fv::testkit::TempDataRoot root;
    const fs::path folder = root.path() / "other-app";
    fs::create_directories(folder);
    const fs::path path = folder / "data.db";
    {
        Connection writer = Connection::open(path, OpenMode::Create);
        writer.exec("PRAGMA journal_mode=WAL");
        writer.exec("CREATE TABLE t (x INTEGER)");
        writer.exec("INSERT INTO t VALUES (1), (2)");
    } // the last close checkpoints and removes -wal and -shm

    REQUIRE_FALSE(fs::exists(folder / "data.db-wal"));
    {
        Connection reader = Connection::open(path, OpenMode::Immutable);
        CHECK(std::get<std::int64_t>(reader.scalar("SELECT COUNT(*) FROM t")) == 2);
        CHECK_THROWS_AS(reader.exec("INSERT INTO t VALUES (3)"), Error);
        CHECK_FALSE(fs::exists(folder / "data.db-wal"));
        CHECK_FALSE(fs::exists(folder / "data.db-shm"));
    }
    CHECK(std::distance(fs::directory_iterator(folder), fs::directory_iterator()) == 1);
}
