#pragma once

#include <filesystem>

#include "backup/backup.h"
#include "base/cancel.h"
#include "base/json.h"

// services/backup.py:957-1067. Blocking: never on the GUI thread.
namespace fv::backup {

// inspect_backup: every member streamed through its checksum, the manifest validated, the
// settings member parsed and the database snapshot checked (integrity, schema, no live meeting,
// every meeting and history path an archived member). Reads .fvbackup files and OpenWhisper's
// .owbackup files (BackupInfo::legacy_format). BackupError for a bundle that fails
// ("Backup file does not exist", "Backup ZIP is corrupt", or the rule it breaks),
// BackupCancelled when `cancel` fires; sql::Error passes through as Python's sqlite3 errors do.
BackupInfo inspect_backup(const std::filesystem::path& archive, const CancelToken& cancel = {});

// _manifest_info: BackupInfo of a validated manifest, with the archive file's size.
BackupInfo manifest_info(const std::filesystem::path& archive, const Json& manifest,
                         bool legacy_format);

} // namespace fv::backup
