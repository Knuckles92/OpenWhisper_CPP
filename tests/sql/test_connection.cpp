// fv_sql: the RAII connection, statements and transactions under fv::Database. No OpenWhisper
// test covers them directly (Python used SQLAlchemy and the sqlite3 module); these pin the
// behaviour the database and the repositories rely on.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "base/cancel.h"
#include "sql/db.h"
#include "testkit/testkit.h"

using fv::sql::Blob;
using fv::sql::Connection;
using fv::sql::Error;
using fv::sql::OpenMode;
using fv::sql::Statement;
using fv::sql::TxMode;
using fv::sql::Value;

namespace {

constexpr int sqlite_busy = 5;
constexpr int sqlite_readonly = 8;
constexpr int sqlite_interrupt = 9;
constexpr int sqlite_cantopen = 14;
constexpr int sqlite_constraint_notnull = 1299;

std::int64_t count(Connection& conn, const std::string& table) {
    return std::get<std::int64_t>(conn.scalar("SELECT count(*) FROM " + table));
}

} // namespace

TEST_CASE("open modes create, require or only read the file") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "modes.db";

    SECTION("ReadWrite and ReadOnly need an existing file") {
        CHECK_THROWS_AS(Connection::open(path, OpenMode::ReadWrite), Error);
        try {
            Connection::open(path, OpenMode::ReadOnly);
            FAIL("opened a missing file read-only");
        } catch (const Error& error) {
            CHECK(error.primary_code() == sqlite_cantopen);
        }
        CHECK_FALSE(std::filesystem::exists(path));
    }

    SECTION("Create makes the file, ReadOnly refuses writes") {
        {
            Connection conn = Connection::open(path, OpenMode::Create);
            conn.exec("CREATE TABLE t (x INTEGER)");
            conn.exec("INSERT INTO t VALUES (1)");
        }
        CHECK(std::filesystem::exists(path));
        Connection read_only = Connection::open(path, OpenMode::ReadOnly);
        CHECK(count(read_only, "t") == 1);
        try {
            read_only.exec("INSERT INTO t VALUES (2)");
            FAIL("wrote through a read-only connection");
        } catch (const Error& error) {
            CHECK(error.primary_code() == sqlite_readonly);
        }
    }
}

TEST_CASE("a read-only URI keeps characters that URIs reserve in the file name") {
    const fv::testkit::TempDataRoot root;
    // '?' would be reserved too, but Windows refuses it in a file name.
    const auto folder = root.path() / std::filesystem::path(u8"odd #1 %20 &x=y \u00e9");
    std::filesystem::create_directories(folder);
    const auto path = folder / "data.db";
    {
        Connection conn = Connection::open(path, OpenMode::Create);
        conn.exec("CREATE TABLE t (x INTEGER); INSERT INTO t VALUES (7);");
    }
    Connection read_only = Connection::open(path, OpenMode::ReadOnly);
    CHECK(std::get<std::int64_t>(read_only.scalar("SELECT x FROM t")) == 7);
}

#ifdef _WIN32
TEST_CASE("a read-only URI opens a file through a UNC path", "[platform]") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "unc.db";
    {
        Connection conn = Connection::open(path, OpenMode::Create);
        conn.exec("CREATE TABLE t (x INTEGER); INSERT INTO t VALUES (3);");
    }
    // \\localhost\<drive>$\... names the same file through the loopback administrative share.
    const std::wstring local = std::filesystem::absolute(path).wstring();
    REQUIRE(local.size() > 3);
    REQUIRE(local[1] == L':');
    const std::filesystem::path unc =
        std::wstring(L"\\\\localhost\\") + local[0] + L"$" + local.substr(2);
    std::error_code error;
    if (!std::filesystem::exists(unc, error)) {
        SKIP("the loopback administrative share is not reachable");
    }
    Connection read_only = Connection::open(unc, OpenMode::ReadOnly);
    CHECK(std::get<std::int64_t>(read_only.scalar("SELECT x FROM t")) == 3);
}
#endif

TEST_CASE("values bind by index and by name and come back with their SQLite type") {
    const fv::testkit::TempDataRoot root;
    Connection conn = Connection::open(root.path() / "values.db", OpenMode::Create);
    conn.exec("CREATE TABLE v (n, i, r, t, b, e)");

    const Blob blob{std::byte{0}, std::byte{0xff}, std::byte{0x10}};
    Statement insert = conn.prepare("INSERT INTO v VALUES (:n, :i, ?3, :t, :b, :e)");
    CHECK(insert.parameter_count() == 6);
    insert.bind(":n", nullptr)
        .bind(":i", std::int64_t{-9'007'199'254'740'993})
        .bind(3, 0.1)
        .bind(":t", std::string("caf\xc3\xa9\0x", 7))
        .bind(":b", blob)
        .bind(":e", Blob{});
    CHECK_FALSE(insert.step());
    CHECK(conn.changes() == 1);
    CHECK(conn.last_insert_rowid() == 1);

    Statement select = conn.prepare("SELECT n, i, r, t, b, e FROM v");
    REQUIRE(select.step());
    CHECK(select.column_count() == 6);
    CHECK(select.column_name(3) == "t");
    CHECK(std::holds_alternative<std::nullptr_t>(select.column(0)));
    CHECK(std::get<std::int64_t>(select.column(1)) == -9'007'199'254'740'993);
    CHECK(std::get<double>(select.column(2)) == 0.1);
    CHECK(std::get<std::string>(select.column(3)) == std::string("caf\xc3\xa9\0x", 7));
    CHECK(std::get<Blob>(select.column(4)) == blob);
    // An empty blob stays a BLOB rather than becoming NULL.
    REQUIRE(std::holds_alternative<Blob>(select.column(5)));
    CHECK(std::get<Blob>(select.column(5)).empty());
    CHECK_FALSE(select.step());

    SECTION("reset keeps the bindings, clear_bindings drops them") {
        insert.reset();
        insert.step();
        CHECK(count(conn, "v") == 2);
        insert.reset();
        insert.clear_bindings();
        insert.step();
        CHECK(
            std::holds_alternative<std::nullptr_t>(conn.scalar("SELECT i FROM v WHERE rowid = 3")));
    }

    SECTION("an unknown name is an error") {
        CHECK_THROWS_WITH(insert.bind(":missing", std::int64_t{1}), "no such parameter: :missing");
    }
}

TEST_CASE("errors carry SQLite's message and extended code") {
    const fv::testkit::TempDataRoot root;
    Connection conn = Connection::open(root.path() / "errors.db", OpenMode::Create);
    conn.exec("CREATE TABLE t (x TEXT NOT NULL)");

    CHECK_THROWS_WITH(conn.prepare("SELECT * FROM missing"), "no such table: missing");
    try {
        conn.exec("INSERT INTO t VALUES (NULL)");
        FAIL("inserted NULL into a NOT NULL column");
    } catch (const Error& error) {
        CHECK(error.code() == sqlite_constraint_notnull);
        CHECK(std::string(error.what()) == "NOT NULL constraint failed: t.x");
    }
    // As Python's sqlite3 Cursor.execute refuses a second statement.
    CHECK_THROWS_WITH(conn.prepare("SELECT 1; SELECT 2"),
                      "You can only execute one statement at a time.");
    CHECK_NOTHROW(conn.prepare("SELECT 1;  \n"));
    CHECK_THROWS_AS(conn.prepare("  "), Error);
}

TEST_CASE("a transaction rolls back unless committed") {
    const fv::testkit::TempDataRoot root;
    Connection conn = Connection::open(root.path() / "tx.db", OpenMode::Create);
    conn.exec("CREATE TABLE t (x INTEGER)");

    {
        auto tx = conn.begin();
        CHECK_FALSE(conn.autocommit());
        conn.exec("INSERT INTO t VALUES (1)");
    }
    CHECK(conn.autocommit());
    CHECK(count(conn, "t") == 0);

    {
        auto tx = conn.begin(TxMode::Exclusive);
        conn.exec("INSERT INTO t VALUES (1)");
        tx.commit();
        CHECK_FALSE(tx.active());
        CHECK_THROWS_AS(tx.commit(), Error);
    }
    CHECK(count(conn, "t") == 1);

    {
        auto tx = conn.begin();
        conn.exec("INSERT INTO t VALUES (2)");
        tx.rollback();
        tx.rollback();
    }
    CHECK(count(conn, "t") == 1);

    SECTION("a moved transaction is finished once") {
        auto first = conn.begin();
        conn.exec("INSERT INTO t VALUES (3)");
        auto second = std::move(first);
        CHECK_FALSE(first.active());
        second.commit();
        CHECK(count(conn, "t") == 2);
    }
}

TEST_CASE("BEGIN IMMEDIATE keeps a second writer out until the busy timeout") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "lock.db";
    Connection first = Connection::open(path, OpenMode::Create);
    first.exec("CREATE TABLE t (x INTEGER)");
    Connection second = Connection::open(path, OpenMode::ReadWrite);
    second.set_busy_timeout(std::chrono::milliseconds(50));

    auto tx = first.begin(TxMode::Immediate);
    try {
        second.begin(TxMode::Immediate);
        FAIL("two writers held the lock");
    } catch (const Error& error) {
        CHECK(error.primary_code() == sqlite_busy);
    }
    tx.commit();
    CHECK_NOTHROW(second.begin(TxMode::Immediate).commit());
}

TEST_CASE("a registered function is called with SQL values and its errors become SQL errors") {
    const fv::testkit::TempDataRoot root;
    Connection conn = Connection::open(root.path() / "fn.db", OpenMode::Create);
    conn.create_function("describe", 1, [](std::span<const Value> args) -> Value {
        return std::visit(
            [](const auto& v) -> Value {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::nullptr_t>) {
                    return std::string("null");
                } else if constexpr (std::is_same_v<T, std::int64_t>) {
                    return v * 2;
                } else if constexpr (std::is_same_v<T, double>) {
                    return v / 2;
                } else if constexpr (std::is_same_v<T, std::string>) {
                    return "<" + v + ">";
                } else {
                    return static_cast<std::int64_t>(v.size());
                }
            },
            args[0]);
    });
    conn.create_function("fails", 0,
                         [](std::span<const Value>) -> Value { throw std::runtime_error("nope"); });

    CHECK(std::get<std::string>(conn.scalar("SELECT describe(NULL)")) == "null");
    CHECK(std::get<std::int64_t>(conn.scalar("SELECT describe(21)")) == 42);
    CHECK(std::get<double>(conn.scalar("SELECT describe(1.5)")) == 0.75);
    CHECK(std::get<std::string>(conn.scalar("SELECT describe('x')")) == "<x>");
    CHECK(std::get<std::int64_t>(conn.scalar("SELECT describe(x'0102')")) == 2);
    CHECK_THROWS_WITH(conn.scalar("SELECT fails()"), "nope");
    // Wrong argument count: the function is not found under that arity.
    CHECK_THROWS_AS(conn.scalar("SELECT describe(1, 2)"), Error);
}

TEST_CASE("backup_to copies the database and stops when cancelled") {
    const fv::testkit::TempDataRoot root;
    Connection source = Connection::open(root.path() / "source.db", OpenMode::Create);
    source.exec("CREATE TABLE t (x TEXT)");
    {
        auto tx = source.begin();
        Statement insert = source.prepare("INSERT INTO t VALUES (?)");
        for (int i = 0; i < 2000; ++i) {
            insert.reset();
            insert.bind(1, std::string(512, static_cast<char>('a' + i % 26)));
            insert.step();
        }
        tx.commit();
    }

    SECTION("a full copy") {
        Connection target = Connection::open(root.path() / "copy.db", OpenMode::Create);
        source.backup_to(target, fv::CancelToken{});
        CHECK(count(target, "t") == 2000);
    }

    SECTION("a cancelled copy throws") {
        Connection target = Connection::open(root.path() / "cancelled.db", OpenMode::Create);
        fv::CancelSource cancel;
        cancel.cancel();
        try {
            source.backup_to(target, cancel.token());
            FAIL("a cancelled backup finished");
        } catch (const Error& error) {
            CHECK(error.code() == sqlite_interrupt);
        }
    }
}

TEST_CASE("a closed or moved-from connection refuses work") {
    const fv::testkit::TempDataRoot root;
    Connection conn = Connection::open(root.path() / "closed.db", OpenMode::Create);
    Connection moved = std::move(conn);
    CHECK_FALSE(conn.is_open());
    CHECK(moved.is_open());
    CHECK_THROWS_WITH(conn.exec("SELECT 1"), "Cannot operate on a closed database.");
    moved.close();
    moved.close();
    CHECK_THROWS_AS(moved.prepare("SELECT 1"), Error);
}
