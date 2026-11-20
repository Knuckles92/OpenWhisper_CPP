#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "backup/backup.h"
#include "backup/names.h"
#include "base/cancel.h"
#include "base/identity.h"
#include "base/json.h"

// services/backup.py:1070-1426: a bundle staged while the app runs and applied at the next
// startup, before settings or SQLite open. Only RESTORE_NAMES are ever moved; the data they
// replace is kept under <restore_dir>/previous-<UTC>-<8 hex>. Blocking: never on the GUI thread.
// apply_pending_restore needs neither the database layer (fv_db) nor Qt.
namespace fv::backup {

// prepare_restore: the bundle inspected in full, then its members extracted and verified into
// <root>/<restore_dir>/stage-<32 hex>/data under identity()'s names (an OpenWhisper .owbackup's
// openwhisper.db and openwhisper_settings.json land as openwhisper-cpp.db and
// openwhisper_cpp_settings.json), the database's meeting paths rebased onto `root`, the settings
// sanitized again, the stage manifest written with the staged database's and settings' size and
// SHA-256, and pending.json written last. Returns the stage folder. Progress reports "stage"
// after each member. Nothing is left behind on failure. BackupError for every refusal ("A
// restore is already waiting for restart", "An earlier restore requires recovery before staging
// another", "End the live meeting before preparing a restore", "Not enough free disk space for a
// safe backup or restore", what inspect_backup refuses, ...), BackupCancelled on cancel.
std::filesystem::path prepare_restore(const std::filesystem::path& archive,
                                      const std::filesystem::path& root,
                                      const Progress& progress = {},
                                      const CancelToken& cancel = {});

// pending_restore_exists: pending.json or journal.json is in the restore folder ("Restore
// directory is a symbolic link" when that folder is a link).
bool pending_restore_exists(const std::filesystem::path& root, const AppIdentity& app = identity());

// previous_data_dir: where the last applied restore kept the data it replaced, from
// last_restore.json; nullopt when no restore was applied. BackupError "Restore history metadata
// is invalid" for a recorded name that is not previous-[A-Za-z0-9_-]+.
std::optional<std::filesystem::path> previous_data_dir(const std::filesystem::path& root,
                                                       const AppIdentity& app = identity());

// apply_pending_restore: false when nothing is pending. Otherwise the stage is checked again
// (every listed file's size and SHA-256, nothing unlisted, the database's meeting and recording
// paths), the owned data moves to a new previous-* folder and the stage into place under a
// journal, and true is returned. On failure everything moved is rolled back, the stage stays for
// a retry and the error propagates. A journal left by a crash is finished (phase "committed")
// or rolled back ("applying") first. Run it under the data root's exclusive lease.
bool apply_pending_restore(const std::filesystem::path& root, const AppIdentity& app = identity());

// _rebase_database: the staged database at `path` checked (integrity, schema
// `expected_schema`, no live meeting) and its PATH_MARKER meeting paths made absolute below
// `root`; a text-only bundle must hold no audio chunks.
void rebase_database(const std::filesystem::path& path, const std::filesystem::path& root,
                     int expected_schema, bool include_recordings, const Names& names);

// The pieces of staging a restore that importing another data root shares (prepare_restore
// uses them in this order).

// _restore_home: <root>/<restore_dir>.
std::filesystem::path restore_home(const std::filesystem::path& root, const Names& names);

// The restore folder created and free for a new stage: no journal awaiting recovery and no link
// ("An earlier restore requires recovery before staging another"), no pending restore ("A
// restore is already waiting for restart"), and no live meeting in the current database ("End
// the live meeting before preparing a restore").
void check_restore_slot(const std::filesystem::path& root, const Names& names);

// "stage-" + uuid4().hex.
std::string new_stage_name();

// The end of staging: the staged database and settings in <home>/<stage_name>/data hashed into
// `manifest` (staged_db_sha256, staged_db_size, staged_settings_sha256, staged_settings_size,
// the keys apply_pending_restore checks them by), the manifest written into the stage, and
// pending.json ({"stage", "created_at"}) written last, which makes the stage pending.
void publish_stage(const std::filesystem::path& home, std::string_view stage_name, Json manifest,
                   std::string_view created_at, const Names& names);

} // namespace fv::backup
