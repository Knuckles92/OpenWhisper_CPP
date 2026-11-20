#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/identity.h"

// services/backup.py's constants (37-82). The names that carry the product (settings file,
// database, restore folder, format id, extension) come from an AppIdentity; every other name is
// OpenWhisper's, unchanged.
namespace fv::backup {

inline constexpr int kFormatVersion = 1;
inline constexpr std::string_view kManifestName = "manifest.json";
inline constexpr std::string_view kPendingName = "pending.json";
inline constexpr std::string_view kJournalName = "journal.json";
inline constexpr std::string_view kLastName = "last_restore.json";
// A format constant, kept in .fvbackup files too: database paths under the data root are stored
// as this prefix plus the path below the root.
inline constexpr std::string_view kPathMarker = "__OW_BACKUP_DATA__/";

inline constexpr std::uint64_t kMaxFiles = 100'000;
inline constexpr std::uint64_t kMaxTotalBytes = std::uint64_t{1} << 40; // 1 TiB
inline constexpr std::uint64_t kMaxFileBytes = std::uint64_t{1} << 36;  // 64 GiB
inline constexpr std::uint64_t kMaxManifestBytes = std::uint64_t{8} << 20;
inline constexpr std::uint64_t kMaxRatio = 10'000;
inline constexpr std::size_t kChunkSize = std::size_t{1} << 20;
// Free space required beyond the data itself before writing an archive or stage.
inline constexpr std::uint64_t kFreeSpaceMargin = std::uint64_t{32} << 20;
// A member's external attributes: a regular file readable only by its owner,
// (stat.S_IFREG | 0o600) << 16.
inline constexpr std::uint32_t kRegularFileAttr = (0100000u | 0600u) << 16;
// Names of a stage and of a kept previous-data folder under the restore folder (regular
// expressions, matched whole).
inline constexpr std::string_view kStageNamePattern = "stage-[0-9a-f]{32}";
inline constexpr std::string_view kPreviousNamePattern = "previous-[A-Za-z0-9_-]+";

// RECORDING_NAMES: owned names whose files are recordings, left out of a text-only bundle and
// the only ones that may hold nested files.
inline constexpr std::array<std::string_view, 6> kRecordingNames = {
    "recorded_audio.wav", ".recorded_audio-recovery", "recordings",
    "meetings",           "remote_records",           "captures"};
// LEGACY_HISTORY_NAMES: merged into the database snapshot, never archived.
inline constexpr std::array<std::string_view, 2> kLegacyHistoryNames = {
    "transcription_history.json", "transcription_history.json.bak"};
inline constexpr std::string_view kLegacyHistoryFile = "transcription_history.json";
inline constexpr std::string_view kRemoteRecords = "remote_records";

bool is_recording_name(std::string_view name);
bool is_legacy_history_name(std::string_view name);

// The names of one product's backups.
struct Names {
    const AppIdentity* identity = nullptr;
    std::string format;        // FORMAT
    std::string extension;     // ARCHIVE_EXTENSION
    std::string settings_file; // fiorivoce_settings.json
    std::string database_file; // fiorivoce.db
    std::string restore_dir;   // RESTORE_DIR
    // OWNED_NAMES, in OpenWhisper's order: settings, database, the legacy history and meeting
    // JSON files with their .bak, recorded_audio.wav, .recorded_audio-recovery, recordings,
    // meetings, captures, remote_records, scratchpad.txt.
    std::vector<std::string> owned;
    // SQLITE_SIDECARS: the database's -wal, -shm and -journal, moved by a restore, never archived.
    std::vector<std::string> sqlite_sidecars;
    // RESTORE_NAMES: owned names, then the sidecars.
    std::vector<std::string> restore_names;
    std::string settings_member; // "data/" + settings_file
    std::string database_member; // "data/" + database_file

    bool is_owned(std::string_view name) const;
    bool is_sqlite_sidecar(std::string_view name) const;
};

// The names of `identity`'s backups; names_for(identity()) writes .fvbackup files and
// names_for(legacy_identity()) reads .owbackup files. The reference lives as long as the
// program.
const Names& names_for(const AppIdentity& identity);

} // namespace fv::backup
