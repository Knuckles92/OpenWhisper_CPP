#pragma once

#include <filesystem>

#include "sql/db.h"

// DatabaseManager's start-up (services/database.py), on the writer connection.
namespace fv::db_detail {

// OpenWhisper's per-connection pragmas plus the 30 s timeout of its create_engine.
// read_only: PRAGMA query_only as well, for the pooled readers.
void configure_connection(sql::Connection&, bool read_only);

// _init_database: migrations of an older file, create_all, legacy meeting tables dropped, the
// meeting FTS index, the delivery trigger and backfill, the schema_version row; then the
// fv_schema_version row. One BEGIN IMMEDIATE transaction, so concurrent first opens of one file
// create the schema once and a failed migration leaves the file as it was.
void init_database(sql::Connection&);

// _migrate_from_json: imports `history_file` when it exists and transcription_history is empty,
// then renames it to <history_file>.bak. Failures are logged, never thrown.
void migrate_from_json(sql::Connection&, const std::filesystem::path& history_file);

} // namespace fv::db_detail
