#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// services/backup.py's public types: portable, offline backups of the data root's owned data
// (create.h, inspect.h). Every message is written for the user.
namespace fv::backup {

// The bundle is invalid or an operation could not complete safely.
class BackupError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A caller cancelled before the bundle or restore was published ("Backup canceled").
class BackupCancelled : public BackupError {
public:
    BackupCancelled() : BackupError("Backup canceled") {}
};

// What a bundle holds, from its manifest (BackupInfo).
struct BackupInfo {
    std::filesystem::path path; // absolute
    std::string created_at;     // UTC ISO 8601 to the second, "+00:00"
    std::string app_version;
    int schema_version = 0;
    bool include_recordings = true;
    std::uint64_t file_count = 0;
    std::uint64_t total_bytes = 0;   // the members' sizes, as the manifest lists them
    std::uint64_t archive_bytes = 0; // the bundle file's size
    std::vector<std::string> warnings;
    // The bundle is OpenWhisper's (.owbackup, format "openwhisper-backup"), its member names
    // read through legacy_identity().
    bool legacy_format = false;

    bool operator==(const BackupInfo&) const = default;
};

// progress(phase, done, total): "scan" once the owned files are listed, then "archive" after
// each member.
using Progress =
    std::function<void(std::string_view phase, std::uint64_t done, std::uint64_t total)>;

} // namespace fv::backup
