#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <vector>

#include "models/detail/fs_native.h"

namespace fv::models::detail {

std::optional<FileIdentity> file_identity(const std::filesystem::path& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return std::nullopt;
    }
    FileIdentity identity;
    identity.volume = static_cast<std::uint64_t>(info.st_dev);
    identity.index = static_cast<std::uint64_t>(info.st_ino);
#if defined(__APPLE__)
    identity.mtime_ns = static_cast<std::int64_t>(info.st_mtimespec.tv_sec) * 1'000'000'000 +
                        info.st_mtimespec.tv_nsec;
#else
    identity.mtime_ns =
        static_cast<std::int64_t>(info.st_mtim.tv_sec) * 1'000'000'000 + info.st_mtim.tv_nsec;
#endif
    identity.size = static_cast<std::int64_t>(info.st_size);
    return identity;
}

std::int64_t walk_size_bytes(const std::filesystem::path& directory) {
    std::int64_t total = 0;
    std::vector<std::filesystem::path> pending{directory};
    while (!pending.empty()) {
        const std::filesystem::path folder = std::move(pending.back());
        pending.pop_back();
        DIR* listing = ::opendir(folder.c_str());
        if (listing == nullptr) {
            continue;
        }
        while (const dirent* entry = ::readdir(listing)) {
            const std::string name(entry->d_name);
            if (name == "." || name == "..") {
                continue;
            }
            const auto child = folder / name;
            struct stat info {};
            if (::lstat(child.c_str(), &info) != 0) {
                continue;
            }
            if (S_ISDIR(info.st_mode)) {
                pending.push_back(child);
            } else {
                total += static_cast<std::int64_t>(info.st_size);
            }
        }
        ::closedir(listing);
    }
    return total;
}

int python_errno(const std::error_code& error) {
    return error.value();
}

int python_winerror(const std::error_code&) {
    return 0;
}

std::uint64_t current_pid() {
    return static_cast<std::uint64_t>(::getpid());
}

std::string text_mode(std::string_view text) {
    return std::string(text);
}

FileLock::FileLock(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "could not open the lock file");
    }
    while (::flock(fd, LOCK_EX) != 0) {
        if (errno != EINTR) {
            const int code = errno;
            ::close(fd);
            throw std::system_error(code, std::generic_category(), "could not lock the lock file");
        }
    }
    handle_ = fd;
}

FileLock::~FileLock() {
    if (handle_ != -1) {
        ::flock(static_cast<int>(handle_), LOCK_UN);
        ::close(static_cast<int>(handle_));
    }
}

} // namespace fv::models::detail
