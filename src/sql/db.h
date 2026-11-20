#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "base/cancel.h"

// SQLite's handles, declared so that this header does not include sqlite3.h (architecture 10.2).
struct sqlite3;
struct sqlite3_stmt;

namespace fv::sql {

using Blob = std::vector<std::byte>;
// One SQLite value: NULL, INTEGER, REAL, TEXT (UTF-8) or BLOB.
using Value = std::variant<std::nullptr_t, std::int64_t, double, std::string, Blob>;

// A registered SQL function; a thrown exception becomes the SQL error.
using ScalarFn = std::function<Value(std::span<const Value> args)>;

// A failed SQLite call. what() is SQLite's message ("no such table: x"), as Python's sqlite3
// errors carry it.
class Error : public std::runtime_error {
public:
    Error(int code, const std::string& message) : std::runtime_error(message), code_(code) {}
    // SQLite's extended result code (SQLITE_BUSY = 5, SQLITE_CONSTRAINT_NOTNULL = 1299, ...).
    int code() const noexcept { return code_; }
    // The primary result code: code() & 0xff.
    int primary_code() const noexcept { return code_ & 0xff; }

private:
    int code_;
};

enum class OpenMode {
    ReadOnly,  // file: URI with mode=ro
    ReadWrite, // the file must exist
    Create,    // created when missing
    // file: URI with mode=ro&immutable=1: no locks, and no -wal, -shm or -journal is read or
    // created. Only for a file nothing writes meanwhile, without a journal to recover.
    Immutable,
};

// BEGIN DEFERRED, BEGIN IMMEDIATE where Python uses it, BEGIN EXCLUSIVE.
enum class TxMode { Deferred, Immediate, Exclusive };

class Connection;

// A prepared statement. Parameters bind by 1-based index or by name (":name", as the SQL text
// SQLAlchemy emits).
// Affinity: the thread of its connection.
class Statement {
public:
    Statement(Statement&&) noexcept;
    Statement& operator=(Statement&&) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement();

    Statement& bind(int index, const Value&);
    Statement& bind(std::string_view name, const Value&);
    bool step(); // true while a row is available
    int column_count() const;
    std::string_view column_name(int index) const;
    Value column(int index) const;
    void reset(); // keeps the bindings

    // The parameter count, and every binding back to NULL.
    int parameter_count() const;
    void clear_bindings();

private:
    friend class Connection;
    Statement(sqlite3* db, sqlite3_stmt* stmt) : db_(db), stmt_(stmt) {}

    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

// RAII: rolls back on destruction unless committed.
// Affinity: the thread of its connection.
class Transaction {
public:
    Transaction(Transaction&&) noexcept;
    Transaction& operator=(Transaction&&) noexcept;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction();

    void commit();
    void rollback();
    // True until commit() or rollback().
    bool active() const noexcept { return connection_ != nullptr; }

private:
    friend class Connection;
    explicit Transaction(Connection* connection) : connection_(connection) {}

    Connection* connection_ = nullptr;
};

// RAII connection. A new connection waits up to 5 s for a lock (Python sqlite3's default
// timeout) and has extended result codes on.
// Affinity: one thread at a time.
class Connection {
public:
    Connection(Connection&&) noexcept;
    Connection& operator=(Connection&&) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection();

    static Connection open(const std::filesystem::path&, OpenMode); // ro (URI), rw, create
    Statement prepare(std::string_view sql);
    void exec(std::string_view sql);
    Transaction begin(TxMode = TxMode::Deferred); // TxMode::Immediate where Python uses it
    void backup_to(Connection& target, const CancelToken&);
    void create_function(std::string_view name, int args, ScalarFn);

    // How long a statement waits for another connection's lock before SQLITE_BUSY.
    void set_busy_timeout(std::chrono::milliseconds);
    // Rows changed by the last INSERT, UPDATE or DELETE, and the rowid the last INSERT made.
    std::int64_t changes() const;
    std::int64_t last_insert_rowid() const;
    // False inside BEGIN ... COMMIT.
    bool autocommit() const;
    // Closes now rather than at destruction; statements must be gone first. Idempotent.
    void close();
    bool is_open() const noexcept { return db_ != nullptr; }

    // The first column of the first row, or nullptr when there is no row.
    Value scalar(std::string_view sql);

private:
    explicit Connection(sqlite3* db) : db_(db) {}

    sqlite3* db_ = nullptr;
};

} // namespace fv::sql
