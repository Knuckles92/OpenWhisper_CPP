#pragma once

#include <sqlite3.h>

#include <string>

#include "sql/db.h"

namespace fv::sql::detail {

// The error of the last failed call on `db`, or SQLite's text for `rc` when there is no handle.
[[noreturn]] inline void throw_error(sqlite3* db, int rc) {
    if (db != nullptr) {
        const int extended = sqlite3_extended_errcode(db);
        throw Error((extended & 0xff) == (rc & 0xff) ? extended : rc, sqlite3_errmsg(db));
    }
    throw Error(rc, sqlite3_errstr(rc));
}

inline void check(sqlite3* db, int rc) {
    if (rc != SQLITE_OK) {
        throw_error(db, rc);
    }
}

} // namespace fv::sql::detail
