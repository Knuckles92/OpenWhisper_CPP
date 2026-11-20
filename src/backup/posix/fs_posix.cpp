#include "backup/detail/fs.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

#include "base/utf8.h"

namespace fv::backup::detail {

namespace {

[[noreturn]] void throw_errno(std::string_view what, const std::filesystem::path& path) {
    throw std::filesystem::filesystem_error(std::string(what), path,
                                            std::error_code(errno, std::generic_category()));
}

FileStat from_stat(const struct stat& info) {
    FileStat result;
    result.device = static_cast<std::uint64_t>(info.st_dev);
    result.inode = static_cast<std::uint64_t>(info.st_ino);
    result.size = static_cast<std::uint64_t>(info.st_size);
    result.modified = static_cast<std::int64_t>(info.st_mtim.tv_sec) * 1'000'000'000 +
                      static_cast<std::int64_t>(info.st_mtim.tv_nsec);
    result.regular = S_ISREG(info.st_mode);
    return result;
}

} // namespace

bool is_link(const std::filesystem::path& path) {
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0 && S_ISLNK(info.st_mode);
}

FileStat stat(const std::filesystem::path& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        throw_errno("stat", path);
    }
    return from_stat(info);
}

void fsync_file(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        throw_errno("open", path);
    }
    const int status = ::fsync(fd);
    const int saved = errno;
    ::close(fd);
    if (status != 0) {
        errno = saved;
        throw_errno("fsync", path);
    }
}

void fsync_directory(const std::filesystem::path& directory) {
    const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    // Some file systems do not support fsync on a directory.
    (void)::fsync(fd);
    ::close(fd);
}

struct InputFile::Impl {
    int fd = -1;
    std::filesystem::path path;
};

InputFile::InputFile(const std::filesystem::path& path) : impl_(std::make_unique<Impl>()) {
    impl_->path = path;
    impl_->fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (impl_->fd < 0) {
        throw_errno("open", path);
    }
}

InputFile::~InputFile() {
    if (impl_ && impl_->fd >= 0) {
        ::close(impl_->fd);
    }
}

FileStat InputFile::stat() const {
    struct stat info {};
    if (::fstat(impl_->fd, &info) != 0) {
        throw_errno("fstat", impl_->path);
    }
    return from_stat(info);
}

std::size_t InputFile::read(std::span<std::byte> buffer) {
    std::size_t total = 0;
    while (total < buffer.size()) {
        const ssize_t got = ::read(impl_->fd, buffer.data() + total, buffer.size() - total);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_errno("read", impl_->path);
        }
        if (got == 0) {
            break;
        }
        total += static_cast<std::size_t>(got);
    }
    return total;
}

} // namespace fv::backup::detail
