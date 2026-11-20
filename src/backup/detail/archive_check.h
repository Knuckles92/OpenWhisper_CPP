#pragma once

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "archive/zip_reader.h"
#include "backup/names.h"
#include "base/cancel.h"
#include "base/json.h"
#include "sql/db.h"

// The validation of a bundle (services/backup.py:852-1062), shared by inspection and by
// staging a restore. Every refusal is a BackupError with OpenWhisper's text.
namespace fv::backup::detail {

// The names a bundle is read with: legacy_identity()'s when its manifest's format is
// OpenWhisper's, identity()'s otherwise (validate_manifest then refuses anything else). Reads
// manifest.json when it has one of at most 8 MiB; any failure there picks identity()'s, and the
// validation proper reports it.
const Names& reading_names(archive::ZipReader& zip);

// _validate_manifest: the archive's members (count, safe names, no case-fold duplicates or
// folders, no link or special mode bits, size and ratio limits), its manifest (format, version,
// metadata), the manifest's file list against the members exactly, the core files, no recording
// in a text-only bundle, and the warnings. Returns the manifest.
Json validate_manifest(archive::ZipReader& zip, const Names& names);

// _verify_member: one listed member streamed through its size and SHA-256 ("Archive member
// exceeds declared size", "Archive checksum mismatch: <path>"); the database member is copied to
// `db_path`. Returns the settings member's bytes, nullopt for any other member.
std::optional<std::string> verify_member(archive::ZipReader& zip, const Json& item,
                                         const std::filesystem::path& db_path,
                                         const CancelToken& cancel, const Names& names);

// _check_settings_json: the settings member is a JSON object.
void check_settings_json(const std::optional<std::string>& settings_bytes);

// _check_archived_meeting_paths: every meeting spool and chunk path is PATH_MARKER +
// "meetings/...", a safe member name, and each chunk's file is one of `assets`.
void check_archived_meeting_paths(sql::Connection& connection, const std::set<std::string>& assets,
                                  const Names& names);

// _check_history_audio: every history recording is a safe relative path to one of `available`.
void check_history_audio(sql::Connection& connection, const std::set<std::string>& available,
                         std::string_view unsafe_message, std::string_view missing_message,
                         const Names& names);

} // namespace fv::backup::detail
