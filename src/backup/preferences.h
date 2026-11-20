#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "backup/backup.h"
#include "base/identity.h"
#include "base/json.h"

// The automatic-backup schedule of services/backup_manager.py: backup-preferences.json in the
// data root, the next due time and the retention of automatic archives. Times are seconds since
// the Unix epoch (time.time()).
namespace fv::backup {

// PREFERENCES_FILE.
inline constexpr std::string_view kPreferencesFile = "backup-preferences.json";
// A preferences file larger than this is ignored.
inline constexpr std::uint64_t kMaxPreferencesBytes = 1024 * 1024;
// scheduled_archives keeps at most this many paths on load (archives[-1000:]).
inline constexpr std::size_t kMaxScheduledArchives = 1000;

// _INTERVALS: "daily" and "weekly" in seconds; nullopt for "off" or anything else.
std::optional<std::int64_t> schedule_interval(std::string_view frequency);

// The coordinator's preferences, in the file's key order.
struct BackupPreferences {
    std::string frequency = "off"; // "off", "daily" or "weekly"
    int retention_count = 5;       // 1..100 automatic archives kept
    std::string destination;       // the folder of automatic archives, resolved; "" when off
    Json last_backup = nullptr;    // the last backup's BackupInfo as a dict, or null
    Json last_auto_at = 0;         // when the last automatic backup finished, as it was stored
    std::vector<std::string> scheduled_archives; // automatic archives made, oldest first

    // The dict as json.dump writes it: frequency, retention_count, destination, last_backup,
    // last_auto_at, scheduled_archives.
    Json to_json() const;
    // The dict as the coordinator holds it, unchecked; a missing key keeps its default.
    static BackupPreferences from_json(const Json& values);
    // last_auto_at as a number (a bool is 0 or 1, as Python's arithmetic reads it).
    double last_auto_seconds() const;
};

// _load_preferences: the file's valid values over the defaults. A missing, linked, unreadable,
// non-object or over-1-MiB file gives the defaults; each value is kept only when valid
// (frequency one of the three, retention_count an int 1..100, destination a string, last_backup
// a dict with a string path, last_auto_at a number from 0 to now + 1 day, the last 1000 string
// entries of scheduled_archives).
BackupPreferences load_preferences(const std::filesystem::path& data_dir, double now);

// _save_preferences: json.dump(indent=2) of to_json() (ASCII, no final newline, CRLF line ends
// on Windows) replacing the file atomically. The data root is created first. BackupError "The
// backup preferences file must not be a symbolic link." when it is one;
// std::filesystem::filesystem_error when it cannot be written.
void save_preferences(const std::filesystem::path& data_dir, const BackupPreferences& preferences);

// snapshot()["next_backup_at"]: nullopt when the schedule is off; otherwise the latest of now,
// the last automatic backup plus the interval (when there was one) and the retry time.
std::optional<double> next_backup_at(const BackupPreferences& preferences, double now,
                                     double retry_after);

// asdict(BackupInfo) with the path as text, as last_backup stores it.
Json backup_info_json(const BackupInfo& info);

// <product>-auto-<YYYYMMDDTHHMMSSZ>-<hex12><extension>, the name of an automatic archive.
std::string auto_backup_name(const AppIdentity& app, std::chrono::system_clock::time_point at,
                             std::string_view hex12);
// _AUTO_NAME.fullmatch(name) for `app`'s product and extension.
bool is_auto_backup_name(std::string_view name, const AppIdentity& app);

// The prune step of _prune_automatic_backups, without its side effects.
struct PruneSelection {
    // The archives to delete: recorded automatic archives that are regular files (not links)
    // directly in the destination, all but the newest retention_count of them.
    std::vector<std::filesystem::path> remove;
    // scheduled_archives without the removed entries.
    std::vector<std::string> remaining;
};
PruneSelection prune_selection(const BackupPreferences& preferences, const AppIdentity& app);

// _prune_automatic_backups without its save: the selected archives deleted in order, then
// scheduled_archives set to the rest. std::filesystem::filesystem_error when one cannot be
// deleted (or is already gone, as Path.unlink() raises), with scheduled_archives unchanged.
void prune_automatic_backups(BackupPreferences& preferences, const AppIdentity& app);

} // namespace fv::backup
