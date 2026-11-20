// Golden vectors of services.database (tools/parity/cases/services.database.py): the schema a
// fresh OpenWhisper database holds, and what OpenWhisper's DatabaseManager makes of the v5, v16
// and v17 fixture files, compared with what fv::Database makes of the same files.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <variant>

#include "base/json.h"
#include "db/database.h"
#include "db/schema.h"
#include "db_test_support.h"
#include "golden/golden.h"
#include "sql/db.h"
#include "testkit/testkit.h"

using fv::Json;
using fv::db_test::dump;
using fv::db_test::normalize_sql;
using fv::db_test::open_and_close;

namespace {

// The stored SQL compared with whitespace runs collapsed, everything else as Python's ==.
void check_same_schema(const Json& expected, Json actual) {
    Json wanted = expected;
    for (Json* master : {&wanted["sqlite_master"], &actual["sqlite_master"]}) {
        for (Json& row : *master) {
            if (row[3].is_string()) {
                row[3] = normalize_sql(row[3].get<std::string>());
            }
        }
    }
    CHECK(wanted["sqlite_master"].dump(1) == actual["sqlite_master"].dump(1));
    CHECK(wanted["table_info"].dump(1) == actual["table_info"].dump(1));
    CHECK(fv::golden::equal(wanted["rows"], actual["rows"]));
    CHECK(wanted["rows"].dump(1) == actual["rows"].dump(1));
    CHECK(fv::golden::equal(wanted["schema_version"], actual["schema_version"]));
}

} // namespace

TEST_CASE("a fresh database has OpenWhisper's schema", "[golden]") {
    fv::golden::for_each("services.database", "fresh_schema", [](const fv::golden::Case& c) {
        const fv::testkit::TempDataRoot root;
        const auto path = root.path() / "fresh.db";
        open_and_close(path);
        const Json actual = dump(path);
        check_same_schema(c.result<Json>(), actual);

        // Byte for byte, not only modulo whitespace: the DDL is SQLAlchemy's text.
        const Json result = c.result<Json>();
        const Json& expected = result["sqlite_master"];
        REQUIRE(expected.size() == actual["sqlite_master"].size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CAPTURE(expected[i][1]);
            CHECK(expected[i][3] == actual["sqlite_master"][i][3]);
        }
    });
}

TEST_CASE("the v5, v16 and v17 fixtures migrate as OpenWhisper migrates them", "[golden]") {
    fv::golden::for_each("services.database", "migrated_schema", [](const fv::golden::Case& c) {
        const fv::testkit::TempDataRoot root;
        const auto path = fv::db_test::copy_fixture(c.arg<std::string>(0), root.path());
        open_and_close(path);
        check_same_schema(c.result<Json>(), dump(path));
        CHECK(c.result<Json>()["schema_version"] == Json::parse("[[18]]"));
    });
}

TEST_CASE("migrating again from a forced-back version changes nothing more", "[golden]") {
    fv::golden::for_each("services.database", "remigrated_schema", [](const fv::golden::Case& c) {
        const fv::testkit::TempDataRoot root;
        const auto path = fv::db_test::copy_fixture(c.arg<std::string>(0), root.path());
        open_and_close(path);
        {
            auto conn = fv::sql::Connection::open(path, fv::sql::OpenMode::ReadWrite);
            auto update = conn.prepare("UPDATE schema_version SET version = ?");
            update.bind(1, c.arg<std::int64_t>(1));
            update.step();
        }
        open_and_close(path);
        check_same_schema(c.result<Json>(), dump(path));
    });
}

TEST_CASE("the fixture of the fresh schema lists the DDL in creation order", "[golden]") {
    // tests/fixtures/db/fresh_v18.sql is sqlite_master.sql ORDER BY rowid of a database
    // OpenWhisper made; fv::Database creates the same objects in the same order.
    std::string expected;
    {
        std::ifstream file(fv::db_test::fixture("fresh_v18.sql"), std::ios::binary);
        expected.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "fresh.db";
    open_and_close(path);
    auto conn = fv::sql::Connection::open(path, fv::sql::OpenMode::ReadOnly);
    auto select = conn.prepare("SELECT sql FROM sqlite_master WHERE sql IS NOT NULL AND name NOT "
                               "LIKE 'fv\\_%' ESCAPE '\\' ORDER BY rowid");
    std::string actual;
    while (select.step()) {
        actual += std::get<std::string>(select.column(0)) + ";\n\n";
    }
    CHECK(actual == expected);
}
