#include <sqlite3.h>

#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

#include "sql/db.h"
#include "sql/detail/check.h"

namespace fv::sql {

namespace {

void bind_value(sqlite3* db, sqlite3_stmt* stmt, int index, const Value& value) {
    const int rc = std::visit(
        [&](const auto& v) -> int {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                return sqlite3_bind_null(stmt, index);
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return sqlite3_bind_int64(stmt, index, v);
            } else if constexpr (std::is_same_v<T, double>) {
                return sqlite3_bind_double(stmt, index, v);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return sqlite3_bind_text64(stmt, index, v.data(), v.size(), SQLITE_TRANSIENT,
                                           SQLITE_UTF8);
            } else {
                // A zero-length blob is a BLOB, not NULL: sqlite3_bind_blob64 reads a null
                // pointer as NULL.
                static const unsigned char empty = 0;
                const void* data = v.empty() ? static_cast<const void*>(&empty) : v.data();
                return sqlite3_bind_blob64(stmt, index, data, v.size(), SQLITE_TRANSIENT);
            }
        },
        value);
    detail::check(db, rc);
}

} // namespace

Statement::Statement(Statement&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)), stmt_(std::exchange(other.stmt_, nullptr)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        sqlite3_finalize(stmt_);
        db_ = std::exchange(other.db_, nullptr);
        stmt_ = std::exchange(other.stmt_, nullptr);
    }
    return *this;
}

Statement::~Statement() {
    sqlite3_finalize(stmt_);
}

Statement& Statement::bind(int index, const Value& value) {
    bind_value(db_, stmt_, index, value);
    return *this;
}

Statement& Statement::bind(std::string_view name, const Value& value) {
    const std::string key(name);
    const int index = sqlite3_bind_parameter_index(stmt_, key.c_str());
    if (index == 0) {
        throw Error(SQLITE_RANGE, "no such parameter: " + key);
    }
    return bind(index, value);
}

bool Statement::step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    detail::throw_error(db_, rc);
}

int Statement::column_count() const {
    return sqlite3_column_count(stmt_);
}

std::string_view Statement::column_name(int index) const {
    const char* name = sqlite3_column_name(stmt_, index);
    return name != nullptr ? std::string_view(name) : std::string_view();
}

Value Statement::column(int index) const {
    switch (sqlite3_column_type(stmt_, index)) {
    case SQLITE_INTEGER:
        return static_cast<std::int64_t>(sqlite3_column_int64(stmt_, index));
    case SQLITE_FLOAT:
        return sqlite3_column_double(stmt_, index);
    case SQLITE_TEXT: {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, index));
        const int size = sqlite3_column_bytes(stmt_, index);
        return std::string(text != nullptr ? text : "", static_cast<std::size_t>(size));
    }
    case SQLITE_BLOB: {
        const void* data = sqlite3_column_blob(stmt_, index);
        const int size = sqlite3_column_bytes(stmt_, index);
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

void Statement::reset() {
    sqlite3_reset(stmt_);
}

int Statement::parameter_count() const {
    return sqlite3_bind_parameter_count(stmt_);
}

void Statement::clear_bindings() {
    sqlite3_clear_bindings(stmt_);
}

} // namespace fv::sql
