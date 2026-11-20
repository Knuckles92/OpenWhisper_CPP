// Database::deferred (services/database.py's _LazyDatabaseManager): nothing touches the file
// until the first use, a start-up that fails leaves it unopened and the next use tries again, so
// an app whose database will not open keeps running with failing saves.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <variant>

#include "db/database.h"
#include "sql/db.h"
#include "testkit/testkit.h"

using fv::Database;

namespace {

std::int64_t count_history(Database& database) {
    return database.read([](fv::sql::Connection& conn) {
        return std::get<std::int64_t>(conn.scalar("SELECT count(*) FROM transcription_history"));
    });
}

} // namespace

TEST_CASE("a deferred database touches nothing until it is used", "[db]") {
    fv::testkit::TempDataRoot root;
    const auto file = root.path() / "openwhisper-cpp.db";
    Database database = Database::deferred(file);
    CHECK_FALSE(database.is_open());
    CHECK_FALSE(std::filesystem::exists(file));
    CHECK(database.path() == file);

    CHECK(count_history(database) == 0);
    CHECK(database.is_open());
    CHECK(std::filesystem::exists(file));
    database.close();
    CHECK_FALSE(database.is_open());
    CHECK_THROWS_AS(count_history(database), fv::sql::Error);
}

TEST_CASE("a deferred database that will not open fails each use and opens once it can", "[db]") {
    fv::testkit::TempDataRoot root;
    // A folder where the database file goes, as test_bootstrap's broken database.
    const auto file = root.path() / "openwhisper-cpp.db";
    std::filesystem::create_directories(file / "x");
    Database database = Database::deferred(file);

    CHECK_THROWS_AS(database.ensure_open(), fv::sql::Error);
    CHECK_FALSE(database.is_open());
    // A save fails, as OpenWhisper's do through its lazy manager; so does a read.
    CHECK_THROWS_AS(database.write([](fv::sql::Connection& conn) {
        conn.exec("INSERT INTO transcription_history (text) VALUES ('x')");
    }),
                    fv::sql::Error);
    CHECK_THROWS_AS(count_history(database), fv::sql::Error);

    // The cause goes away: the next use opens it.
    std::filesystem::remove_all(file);
    CHECK(count_history(database) == 0);
    CHECK(database.is_open());
}

TEST_CASE("a deferred database closed before it opened never opens", "[db]") {
    fv::testkit::TempDataRoot root;
    const auto file = root.path() / "openwhisper-cpp.db";
    Database database = Database::deferred(file);
    database.close();
    CHECK_THROWS_AS(database.ensure_open(), fv::sql::Error);
    CHECK_THROWS_AS(count_history(database), fv::sql::Error);
    CHECK_FALSE(std::filesystem::exists(file));
}
