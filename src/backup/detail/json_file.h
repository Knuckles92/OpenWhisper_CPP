#pragma once

#include <cstdint>
#include <filesystem>

#include "backup/names.h"
#include "base/json.h"

// The small JSON files of a restore (pending.json, journal.json, last_restore.json and a stage's
// manifest.json): services/backup.py:629-670. Blocking: never on the GUI thread.
namespace fv::backup::detail {

// _write_json_atomic: `value` as compact JSON (ensure_ascii=False) and a newline, written in
// text mode (CRLF on Windows) to a temporary file beside `path`, flushed to disk, moved over
// `path`, and the folder's rename persisted where the platform supports it. The folder is
// created first. On any failure `path` is left as it was and
// std::filesystem::filesystem_error is thrown.
void write_json_atomic(const std::filesystem::path& path, const Json& value);

// _read_json: the JSON object in the file at `path`. BackupError "Restore metadata is missing or
// invalid" when it is not a regular file, is a link or is larger than `max_bytes`; "Restore
// metadata is invalid" when it cannot be read or is not UTF-8 JSON; "Restore metadata must be an
// object" otherwise when it is not an object.
Json read_json(const std::filesystem::path& path, std::uint64_t max_bytes = kMaxManifestBytes);

} // namespace fv::backup::detail
