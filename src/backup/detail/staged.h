#pragma once

#include <filesystem>
#include <set>
#include <string>

#include "backup/names.h"
#include "base/json.h"
#include "sql/db.h"

// The checks a pending stage passes before apply_pending_restore moves anything
// (services/backup.py:1201-1308), and the file helpers staging shares. Every refusal is a
// BackupError with OpenWhisper's text. Blocking: never on the GUI thread.
namespace fv::backup::detail {

// _file_sha256: the file's SHA-256 as 64 lowercase hex digits.
std::string file_sha256(const std::filesystem::path& path);

// json.dumps(settings, ensure_ascii=False, indent=2) + "\n" as Path.write_text writes it: CRLF
// line ends on Windows.
std::string settings_text(const Json& settings);

// _check_staged_file: one manifest item's file in <stage>/data has the size and SHA-256 the
// manifest records for it (the staged_db_* and staged_settings_* keys for the database and
// settings, rewritten while staging; the item's own size and sha256 otherwise). Its member name
// joins `listed`, before the file is looked at.
void check_staged_file(const std::filesystem::path& stage, const Json& manifest, const Json& item,
                       std::set<std::string>& listed, const Names& names);

// _staged_tree_files: "data/<path>" of every file below `data_stage` (links refused).
std::set<std::string> staged_tree_files(const std::filesystem::path& data_stage);

// _check_staged_meeting_paths: every meeting spool and chunk path lies below `root`'s meetings
// folder as a safe member, and each chunk's file is one of `found`.
void check_staged_meeting_paths(sql::Connection& connection, const std::filesystem::path& root,
                                const std::set<std::string>& found, const Names& names);

// _restore_stage_check: the stage's manifest, files and database, before anything moves.
void restore_stage_check(const std::filesystem::path& stage, const std::filesystem::path& root,
                         const Names& names);

} // namespace fv::backup::detail
