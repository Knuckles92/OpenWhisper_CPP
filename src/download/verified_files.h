#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "base/clock.h"
#include "base/log.h"

// File digests, free-space checks and lock retries (services/verified_files.py), shared by
// downloads, component installs and the updater. Blocking: never on the GUI thread.
namespace fv::download {

// Read and hash in pieces of this many bytes (1 MiB).
inline constexpr std::size_t kChunkBytes = std::size_t{1} << 20;
// Free space asked for beyond the bytes an operation will write.
inline constexpr double kSpaceMargin = 1.15;

// A path that must be one ordinary file is a link, a directory or a device.
class NotARegularFile : public std::runtime_error {
public:
    explicit NotARegularFile(const std::string& path) : std::runtime_error(path) {}
};

// The lowercase hex SHA-256 of the file. `regular_only` opens it without following a final link
// and throws NotARegularFile for anything but a single-link regular file, so a planted link
// cannot redirect what gets verified. Throws std::system_error when it cannot be read.
std::string sha256_file(const std::filesystem::path& path, bool regular_only = false);

// Free bytes on the drive of `path`, as shutil.disk_usage(path).free reports them (on Windows all
// free bytes, not only those under the caller's quota).
std::uint64_t disk_free_bytes(const std::filesystem::path& path);

// (needed, free) bytes when the drive of `path` is short, else nullopt. `needed` is
// int(required_bytes * margin). `path` is created first, so the check can run before anything
// has been written there. `free_bytes` stands in for disk_free_bytes in tests.
std::optional<std::pair<std::int64_t, std::int64_t>> free_space_shortfall(
    const std::filesystem::path& path, std::int64_t required_bytes, double margin = kSpaceMargin,
    const std::function<std::uint64_t(const std::filesystem::path&)>& free_bytes = {});

// "Not enough disk space: 1.73 GB needed, 1.00 GB free on this drive." (the text component
// installs show for a shortfall, components._check_free_space).
std::string describe_free_space_shortfall(std::int64_t needed, std::int64_t free);

// Whether Windows refused a file operation only because of a lock: access denied (5), sharing
// violation (32) or lock violation (33). Always false elsewhere.
bool is_windows_lock_error(const std::error_code&);

// str() of the OSError Python would raise for the code ("[WinError 32] The process cannot access
// the file because it is being used by another process", "[Errno 28] No space left on device").
std::string os_error_text(const std::error_code&);
// The same for an exception; a filesystem_error adds its paths as Python adds the filenames
// ("...: 'C:\\x'" or "...: 'a' -> 'b'").
std::string os_error_text(const std::system_error&);

struct LockRetryOptions {
    std::chrono::milliseconds poll{250};
    std::function<bool(const std::system_error&)> locked; // empty: is_windows_lock_error
    std::function<void(std::chrono::milliseconds)> wait;  // empty: sleep; a cancel's wait_for
    const Clock* clock = nullptr;                         // null: system_clock()
};

namespace detail {
inline constexpr log::Logger kFilesLog("services.verified_files");
} // namespace detail

// Runs `action`, retrying for up to `timeout` while it fails with a lock error. Antimalware
// scanners open files they have never seen, and a process that has just exited keeps its image
// mapped for a moment; both surface as a brief sharing violation or access denial. Any other
// error, or a lock that outlasts the timeout, is rethrown.
template <class Action>
auto retry_while_locked(std::string_view what, Action&& action, std::chrono::milliseconds timeout,
                        const LockRetryOptions& options = {}) -> decltype(action()) {
    const Clock& clock = options.clock != nullptr ? *options.clock : system_clock();
    const auto deadline = clock.steady() + timeout;
    while (true) {
        try {
            return action();
        } catch (const std::system_error& error) {
            const bool locked =
                options.locked ? options.locked(error) : is_windows_lock_error(error.code());
            if (!locked || clock.steady() >= deadline) {
                throw;
            }
            detail::kFilesLog.info("{} is locked ({}); retrying", what, os_error_text(error));
        }
        if (options.wait) {
            options.wait(options.poll);
        } else {
            std::this_thread::sleep_for(options.poll);
        }
    }
}

} // namespace fv::download
