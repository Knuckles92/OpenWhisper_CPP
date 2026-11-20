#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "backup/backup.h"
#include "base/cancel.h"
#include "base/identity.h"
#include "base/json.h"
#include "base/paths.h"

namespace fv {
class CredentialStore;
}

// Importing an OpenWhisper data folder (architecture 7.5): the restore pipeline pointed at a live
// folder. The folder is staged straight into this app's restore folder, as a restore stages a
// bundle, and apply_pending_restore (restore.h) moves it into place at the next start. Nothing
// is ever written into the OpenWhisper folder: its database is copied out before SQLite opens
// it, recordings are hard-linked or copied out, and the only thing taken there is its data lease,
// exclusively, for the length of the staging (a running OpenWhisper holds it shared). Blocking:
// never on the GUI thread.
namespace fv::backup {

// An OpenWhisper data folder that holds something to import.
struct ImportSource {
    std::filesystem::path root;
    bool has_settings = false;
    bool has_database = false;
    // The history the import brings: the database's entries, or those of
    // transcription_history.json when the database has none (it is merged then).
    std::uint64_t history_entries = 0;
    // The bytes of the recordings it brings: recorded_audio.wav, the recording journals and the
    // recordings, meetings and captures folders.
    std::uint64_t recordings_bytes = 0;

    bool operator==(const ImportSource&) const = default;
};

struct ImportOptions {
    // Copy the API keys and secrets OpenWhisper saved (import_credentials.h) once the stage is
    // published.
    bool copy_credentials = false;
    // The importing build's version: the stage manifest's app_version and fv_import's version.
    std::string app_version;
    // Makes `to` the same file as `from`; empty: std::filesystem::create_hard_link. When it
    // throws (another volume, a file system without hard links) the file is copied.
    std::function<void(const std::filesystem::path& from, const std::filesystem::path& to)>
        link_file;
    // The stores keys are copied between; null: this platform's store under the `from` and `to`
    // identities' credential services.
    CredentialStore* credentials_from = nullptr;
    CredentialStore* credentials_to = nullptr;
};

struct ImportResult {
    std::filesystem::path stage;
    // What the stage left behind (history audio that is missing, a key that could not be
    // copied), for the log.
    std::vector<std::string> warnings;
    // The names the keys were saved under in this app's store.
    std::vector<std::string> copied_keys;
};

// OpenWhisper holds its data lease: it is running and must be closed first.
class OpenWhisperRunning : public BackupError {
public:
    using BackupError::BackupError;
};

// What `root` (a folder of `from`'s data) holds to import, or nullopt when it is not a folder,
// is a link, or holds no settings, database or legacy history. Counts the history with an
// immutable open of OpenWhisper's database, which creates nothing beside it; from a copy in a
// temporary folder only while a -wal or -journal sits beside it (rows may still be there).
std::optional<ImportSource> inspect_import_source(const std::filesystem::path& root,
                                                  const AppIdentity& from = legacy_identity());

// The OpenWhisper data folders of paths.openwhisper_roots() that hold something to import, in
// that order (OPENWHISPER_DATA_DIR first, then OpenWhisper's per-user folder).
std::vector<ImportSource> find_openwhisper_data(const Paths& paths);

// Whether another process holds `from`'s data lease on `root` (a running OpenWhisper). Takes the
// lease exclusively for an instant to find out; false when the folder does not exist.
bool openwhisper_running(const std::filesystem::path& root,
                         const AppIdentity& from = legacy_identity());

// The settings an import stages: `legacy` (OpenWhisper's settings object) with a
// selected_model that this app cannot read (an engine it dropped, or anything else that is not
// one of its choices or aliases) stored as "parakeet", every other key kept, then the restore
// sanitizer (sanitize.h), then fv_import = {source, at, version}.
Json import_settings(const Json& legacy, std::string_view source, std::string_view at,
                     std::string_view version);

// stage_from_data_root: `source` (a data folder of `from`) staged as a pending restore of `root`
// (a data folder of `to`), under `to`'s names, in
// <root>/<restore_dir>/stage-<32 hex>/data with the stage manifest apply_pending_restore checks,
// and pending.json written last:
// - the database copied out and taken through SQLite's online backup, the legacy
//   transcription_history.json merged into it when its history table is empty, its meeting
//   paths and absolute history paths made relative to `source` and then rebased onto `root`, as
//   a bundle's are, and its meeting tokens renewed;
// - the settings as import_settings makes them; settings JSON that cannot be read (not UTF-8,
//   not JSON, not an object) imports as {} with a warning, as OpenWhisper starts on its defaults,
//   where a link or a folder in its place is still refused;
// - a legacy transcription_history.json that cannot be merged is left out with a warning, as
//   OpenWhisper's own migration skips it;
// - recordings, meetings and captures hard-linked (copied where linking fails); the files either
//   app rewrites in place (recorded_audio.wav, scratchpad.txt, the meeting JSON files, the
//   recording journals) copied;
// - never the legacy history files, remote_records or the database's -wal, -shm and -journal.
// Holds `from`'s lease on `source` exclusively throughout (OpenWhisperRunning when it is held).
// Then, when options.copy_credentials, copies the saved keys from `from`'s names into `to`'s
// (failures become warnings; one warning when `from`'s store cannot be read at all).
// Progress reports "stage" after each file. Nothing is left in `root` on failure. BackupError for
// every refusal (what check_restore_slot refuses, "A restore is already waiting for restart"
// among them), BackupCancelled on cancel.
ImportResult stage_from_data_root(const AppIdentity& from, const std::filesystem::path& source,
                                  const AppIdentity& to, const std::filesystem::path& root,
                                  ImportOptions options = {}, Progress progress = {},
                                  CancelToken cancel = {});

} // namespace fv::backup
