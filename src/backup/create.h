#pragma once

#include <filesystem>
#include <string>

#include "backup/backup.h"
#include "base/cancel.h"

// services/backup.py:629-845. Blocking: never on the GUI thread.
namespace fv::backup {

struct CreateOptions {
    // The data root to back up; empty means the resolved OpenWhisper_CPP data root.
    std::filesystem::path data_root;
    bool include_recordings = true;
    // The manifest's app_version (the running build's version).
    std::string app_version;
    Progress progress;
    CancelToken cancel;
};

// create_backup: a .fvbackup of the owned data at `destination`, written under identity()'s
// names. In order: a SQLite online-backup snapshot of the database (or a fresh one), the legacy
// history merged into it, the snapshot sanitized, the owned files listed, the settings read
// under a file signature and sanitized, the size and free-space checks, the bundle written to a
// ".fvbackup-build-*" folder beside the destination, inspected in full, every source checked
// unchanged, and only then moved over `destination`. Nothing is published on any failure or
// cancel (BackupCancelled); an existing destination is left as it was.
//
// BackupError with the rule that failed ("Backup file must end in .fvbackup", "Choose a backup
// destination outside recorded data", "End the live meeting before creating a backup",
// "Data changed during backup: <name>", "Not enough free disk space for a safe backup or
// restore", ...); file errors (std::filesystem::filesystem_error) and sql::Error pass through
// as Python's OSError and sqlite3 errors do.
BackupInfo create_backup(const std::filesystem::path& destination, const CreateOptions& options);

} // namespace fv::backup
