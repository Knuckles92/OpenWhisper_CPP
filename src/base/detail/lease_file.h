#pragma once

#include <cstdint>
#include <filesystem>

// The platform half of DataLease: one implementation in win/, one in posix/.
namespace fv::detail {

inline constexpr std::intptr_t kNoFile = -1;

// Opens (creating, mode 0600) the lock file for reading and writing, without following a
// symbolic link where the platform can refuse one. Throws std::filesystem::filesystem_error when
// it cannot be opened, StartupRestoreError when it is not a regular file. Writes one NUL byte
// when the file is empty.
std::intptr_t open_lease_file(const std::filesystem::path& path);

// Locks byte 0 (length 1) without waiting; false when the lock is not granted.
bool lock_lease_file(std::intptr_t file, bool exclusive);

// Unlocks byte 0 and closes the file, ignoring an unlock failure.
void unlock_and_close_lease_file(std::intptr_t file);
void close_lease_file(std::intptr_t file);

// os.path.normcase: lowercase with backslashes on Windows, unchanged elsewhere.
std::filesystem::path normcase(const std::filesystem::path& path);

} // namespace fv::detail
