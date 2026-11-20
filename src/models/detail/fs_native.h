#pragma once

#include <compare>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace fv::models::detail {

// What os.stat tells apart between two installs of one path: the file's identity (st_dev,
// st_ino), its modification time in nanoseconds and its size.
struct FileIdentity {
    std::uint64_t volume = 0;
    std::uint64_t index = 0;
    std::int64_t mtime_ns = 0;
    std::int64_t size = 0;
    bool operator==(const FileIdentity&) const = default;
};

// The identity of `path` (a file or a directory, following a final link as os.stat does), or
// nullopt when it cannot be read.
std::optional<FileIdentity> file_identity(const std::filesystem::path& path);

// _walk_size_bytes: bytes of every file under `directory`, sizes as the directory listing gives
// them; links are counted as themselves (never followed), unreadable entries skipped.
std::int64_t walk_size_bytes(const std::filesystem::path& directory);

// The errno Python's OSError carries for this error: on Windows the C runtime's mapping of the
// Windows error (5 and 19 to 36 are EACCES, 112 is ENOSPC, ...), elsewhere the value itself.
int python_errno(const std::error_code& error);
// The Windows error itself (OSError.winerror), or 0 off Windows.
int python_winerror(const std::error_code& error);

// os.getpid().
std::uint64_t current_pid();

// Text as Python's text mode writes it: "\n" becomes "\r\n" on Windows.
std::string text_mode(std::string_view text);

// An exclusive lock on `path` across processes (filelock.FileLock): blocks until it is taken;
// the file is created when missing. Throws std::system_error when it cannot be opened. On
// Windows the last holder removes the file, as filelock's WindowsFileLock does; elsewhere it
// stays, as with filelock's UnixFileLock.
// Affinity: one thread at a time.
class FileLock {
public:
    explicit FileLock(const std::filesystem::path& path);
    ~FileLock();
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

private:
    std::intptr_t handle_ = -1;
#ifdef _WIN32
    std::filesystem::path path_; // removed on release
#endif
};

} // namespace fv::models::detail
