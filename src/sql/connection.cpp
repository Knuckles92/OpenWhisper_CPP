#include <sqlite3.h>

#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "base/utf8.h"
#include "sql/db.h"
#include "sql/detail/check.h"

namespace fv::sql {

namespace {

// Python sqlite3's default timeout.
constexpr std::chrono::milliseconds default_busy_timeout{5000};
// Pages copied per backup step; the cancel token is checked between steps.
constexpr int backup_pages_per_step = 1024;
// Python sqlite3's backup `sleep` when the source is locked.
constexpr std::chrono::milliseconds backup_retry_wait{250};

// A file: URI for SQLite's URI filenames: forward slashes, "/" before a drive letter, and the
// characters URI syntax gives a meaning percent-encoded.
std::string file_uri(const std::filesystem::path& path) {
    std::string text = path_to_utf8(std::filesystem::absolute(path));
    for (char& c : text) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (text.starts_with("//")) {
        // A UNC path (\\server\share\...): "file://server/..." would name "server" as the URI
        // authority, which SQLite refuses; an empty authority keeps the whole path.
        text.insert(0, "//");
    } else if (!text.empty() && text.front() != '/') {
        text.insert(text.begin(), '/');
    }
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string uri = "file:";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '%' || c == '?' || c == '#' || byte <= 0x20) {
            uri += '%';
            uri += hex[byte >> 4];
            uri += hex[byte & 0x0f];
        } else {
            uri += c;
        }
    }
    return uri + "?mode=ro";
}

Value value_of(sqlite3_value* value) {
    switch (sqlite3_value_type(value)) {
    case SQLITE_INTEGER:
        return static_cast<std::int64_t>(sqlite3_value_int64(value));
    case SQLITE_FLOAT:
        return sqlite3_value_double(value);
    case SQLITE_TEXT: {
        const auto* text = reinterpret_cast<const char*>(sqlite3_value_text(value));
        const int size = sqlite3_value_bytes(value);
        return std::string(text != nullptr ? text : "", static_cast<std::size_t>(size));
    }
    case SQLITE_BLOB: {
        const void* data = sqlite3_value_blob(value);
        const int size = sqlite3_value_bytes(value);
        Blob blob(static_cast<std::size_t>(size));
        if (size > 0) {
            std::memcpy(blob.data(), data, blob.size());
        }
        return blob;
    }
    default:
        return nullptr;
    }
}

void set_result(sqlite3_context* context, const Value& value) {
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                sqlite3_result_null(context);
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                sqlite3_result_int64(context, v);
            } else if constexpr (std::is_same_v<T, double>) {
                sqlite3_result_double(context, v);
            } else if constexpr (std::is_same_v<T, std::string>) {
                sqlite3_result_text64(context, v.data(), v.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
            } else if (v.empty()) {
                sqlite3_result_zeroblob(context, 0);
            } else {
                sqlite3_result_blob64(context, v.data(), v.size(), SQLITE_TRANSIENT);
            }
        },
        value);
}

void call_function(sqlite3_context* context, int argc, sqlite3_value** argv) {
    const auto* fn = static_cast<const ScalarFn*>(sqlite3_user_data(context));
    try {
        std::vector<Value> args;
        args.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            args.push_back(value_of(argv[i]));
        }
        set_result(context, (*fn)(args));
    } catch (const std::bad_alloc&) {
        sqlite3_result_error_nomem(context);
    } catch (const std::exception& error) {
        sqlite3_result_error(context, error.what(), -1);
    } catch (...) {
        sqlite3_result_error(context, "unknown error in a user-defined function", -1);
    }
}

void destroy_function(void* fn) {
    delete static_cast<ScalarFn*>(fn);
}

bool only_blank(const char* text, const char* end) {
    for (; text < end; ++text) {
        if (*text != ' ' && *text != '\t' && *text != '\r' && *text != '\n' && *text != ';') {
            return false;
        }
    }
    return true;
}

sqlite3* require(sqlite3* db) {
    if (db == nullptr) {
        throw Error(SQLITE_MISUSE, "Cannot operate on a closed database.");
    }
    return db;
}

} // namespace

Connection::Connection(Connection&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        close();
        db_ = std::exchange(other.db_, nullptr);
    }
    return *this;
}

Connection::~Connection() {
    close();
}

Connection Connection::open(const std::filesystem::path& path, OpenMode mode) {
    std::string name;
    int flags = SQLITE_OPEN_NOMUTEX;
    switch (mode) {
    case OpenMode::ReadOnly:
        name = file_uri(path);
        flags |= SQLITE_OPEN_READONLY | SQLITE_OPEN_URI;
        break;
    case OpenMode::Immutable:
        name = file_uri(path) + "&immutable=1";
        flags |= SQLITE_OPEN_READONLY | SQLITE_OPEN_URI;
        break;
    case OpenMode::ReadWrite:
        name = path_to_utf8(path);
        flags |= SQLITE_OPEN_READWRITE;
        break;
    case OpenMode::Create:
        name = path_to_utf8(path);
        flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
        break;
    }
    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(name.c_str(), &db, flags, nullptr);
    if (rc != SQLITE_OK) {
        if (db == nullptr) {
            throw std::bad_alloc();
        }
        Error error(sqlite3_extended_errcode(db), sqlite3_errmsg(db));
        sqlite3_close_v2(db);
        throw error;
    }
    Connection connection(db);
    sqlite3_extended_result_codes(db, 1);
    connection.set_busy_timeout(default_busy_timeout);
    return connection;
}

Statement Connection::prepare(std::string_view sql) {
    sqlite3* db = require(db_);
    sqlite3_stmt* stmt = nullptr;
    const char* tail = nullptr;
    const int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt, &tail);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        detail::throw_error(db, rc);
    }
    Statement statement(db, stmt);
    if (stmt == nullptr) {
        throw Error(SQLITE_MISUSE, "The SQL text holds no statement.");
    }
    if (tail != nullptr && !only_blank(tail, sql.data() + sql.size())) {
        throw Error(SQLITE_MISUSE, "You can only execute one statement at a time.");
    }
    return statement;
}

void Connection::exec(std::string_view sql) {
    sqlite3* db = require(db_);
    const std::string text(sql);
    detail::check(db, sqlite3_exec(db, text.c_str(), nullptr, nullptr, nullptr));
}

Transaction Connection::begin(TxMode mode) {
    switch (mode) {
    case TxMode::Deferred:
        exec("BEGIN DEFERRED");
        break;
    case TxMode::Immediate:
        exec("BEGIN IMMEDIATE");
        break;
    case TxMode::Exclusive:
        exec("BEGIN EXCLUSIVE");
        break;
    }
    return Transaction(this);
}

void Connection::backup_to(Connection& target, const CancelToken& cancel) {
    sqlite3* source = require(db_);
    sqlite3* destination = require(target.db_);
    sqlite3_backup* backup = sqlite3_backup_init(destination, "main", source, "main");
    if (backup == nullptr) {
        detail::throw_error(destination, sqlite3_errcode(destination));
    }
    int rc = SQLITE_OK;
    bool cancelled = false;
    for (;;) {
        if (cancel.cancelled()) {
            cancelled = true;
            break;
        }
        rc = sqlite3_backup_step(backup, backup_pages_per_step);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            if (cancel.wait_for(backup_retry_wait)) {
                cancelled = true;
                break;
            }
            continue;
        }
        if (rc != SQLITE_OK) {
            break;
        }
    }
    const int finish = sqlite3_backup_finish(backup);
    if (cancelled) {
        throw Error(SQLITE_INTERRUPT, "The backup was cancelled.");
    }
    if (rc != SQLITE_DONE) {
        detail::throw_error(destination, rc);
    }
    detail::check(destination, finish);
}

void Connection::create_function(std::string_view name, int args, ScalarFn fn) {
    sqlite3* db = require(db_);
    const std::string key(name);
    auto holder = std::make_unique<ScalarFn>(std::move(fn));
    // sqlite3_create_function_v2 calls destroy_function itself when it fails.
    const int rc = sqlite3_create_function_v2(db, key.c_str(), args, SQLITE_UTF8, holder.release(),
                                              &call_function, nullptr, nullptr, &destroy_function);
    detail::check(db, rc);
}

void Connection::set_busy_timeout(std::chrono::milliseconds timeout) {
    sqlite3* db = require(db_);
    detail::check(db, sqlite3_busy_timeout(db, static_cast<int>(timeout.count())));
}

std::int64_t Connection::changes() const {
    return sqlite3_changes64(require(db_));
}

std::int64_t Connection::last_insert_rowid() const {
    return sqlite3_last_insert_rowid(require(db_));
}

bool Connection::autocommit() const {
    return sqlite3_get_autocommit(require(db_)) != 0;
}

void Connection::close() {
    if (db_ != nullptr) {
        // close_v2 defers the close until the last statement is finalized.
        sqlite3_close_v2(std::exchange(db_, nullptr));
    }
}

Value Connection::scalar(std::string_view sql) {
    Statement statement = prepare(sql);
    return statement.step() ? statement.column(0) : Value(nullptr);
}

} // namespace fv::sql
