#pragma once

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "backup/names.h"
#include "base/cancel.h"
#include "sql/db.h"

// The database side of a bundle (services/backup.py:363-584): a consistent snapshot through
// SQLite's online backup, then its paths made portable and its secrets regenerated. Blocking:
// never on the GUI thread.
namespace fv::backup {

// _table, _columns, _db_schema.
bool has_table(sql::Connection& connection, std::string_view table);
std::set<std::string> columns(sql::Connection& connection, std::string_view table);
// schema_version's version, 0 when the table or its row is missing.
int db_schema(sql::Connection& connection);

// _quick_check: PRAGMA quick_check is "ok" ("Backup database failed SQLite integrity check")
// and PRAGMA foreign_key_check finds nothing ("Backup database has broken foreign keys").
void quick_check(sql::Connection& connection);

// _meeting_is_live: a meeting_sessions row is "active" or "paused".
bool meeting_is_live(sql::Connection& connection);

// _snapshot_database: the database at `source` copied to `output` through the online backup,
// read through the read-only URI ("End the live meeting before creating a backup" while a
// meeting is live); without a source, a fresh database of the current schema. Cancel is checked
// between backup steps (BackupCancelled). Returns the snapshot's schema version.
int snapshot_database(const std::filesystem::path& source, const std::filesystem::path& output,
                      const CancelToken& cancel);

// _relative_under: `path_text` (relative ones are taken below `root`) as the "/"-separated path
// below root, when it lies below root and its first part is `group`; nullopt otherwise. The
// result is checked as a member name (BackupError when unsafe).
std::optional<std::string> relative_under(std::string_view path_text,
                                          const std::filesystem::path& root, std::string_view group,
                                          const Names& names);

// _meeting_session_updates: the portable spool path (PATH_MARKER + rel), then a fresh
// secrets.token_urlsafe(32) for each of host_token and guest_token, then NULL for each of
// asr_remote_json, agent_endpoint_json, app_pid and app_heartbeat_at, for the columns the table
// has.
std::vector<std::pair<std::string, sql::Value>>
meeting_session_updates(const std::set<std::string>& table_columns, std::string_view rel);

struct SanitizedDatabase {
    int schema_version = 0;
    std::vector<std::string> warnings;
    // Members the rewritten rows now depend on ("data/recordings/...", "data/meetings/...").
    std::set<std::string> required_assets;
};

// _sanitize_database: the snapshot at `path` rewritten in place for a bundle of `source_root`'s
// data: meeting spool and chunk paths below PATH_MARKER, meeting tokens regenerated and live
// endpoints cleared, record_sync emptied; a text-only bundle drops audio chunks and history
// audio. Refuses a schema newer than this app ("Database schema is newer than this app").
SanitizedDatabase sanitize_database(const std::filesystem::path& path,
                                    const std::filesystem::path& source_root,
                                    bool include_recordings, const Names& names);

} // namespace fv::backup
