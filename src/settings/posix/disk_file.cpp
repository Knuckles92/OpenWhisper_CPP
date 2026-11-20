#include "settings/detail/disk_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "base/utf8.h"
#include "pyport/text.h"

namespace fv::detail {

namespace {

std::int64_t nanoseconds(const timespec& time) {
    return static_cast<std::int64_t>(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}

[[noreturn]] void throw_errno(const std::filesystem::path& path, int code) {
    const char* type = "OSError";
    switch (code) {
    case EACCES:
    case EPERM:
        type = "PermissionError";
        break;
    case ENOENT:
        type = "FileNotFoundError";
        break;
    case EISDIR:
        type = "IsADirectoryError";
        break;
    default:
        break;
    }
    throw SettingsFileError(type, "[Errno " + std::to_string(code) + "] " + std::strerror(code) +
                                      ": " + py::repr(path_to_utf8(path)));
}

class Descriptor {
public:
    explicit Descriptor(int fd) : fd_(fd) {}
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    int get() const { return fd_; }

private:
    int fd_;
};

} // namespace

std::optional<FileSignature> stat_signature(const std::filesystem::path& absolute) {
    struct stat info {};
    if (::stat(absolute.c_str(), &info) != 0) {
        if (errno == ENOENT) {
            return FileSignature{.path = absolute};
        }
        return std::nullopt;
    }
    return FileSignature{
        .path = absolute,
        .exists = true,
        .volume = static_cast<std::uint64_t>(info.st_dev),
        .index = static_cast<std::uint64_t>(info.st_ino),
        .size = static_cast<std::uint64_t>(info.st_size),
        .modified = nanoseconds(info.st_mtim),
        .changed = nanoseconds(info.st_ctim),
    };
}

std::string read_whole_file(const std::filesystem::path& path) {
    const Descriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (file.get() < 0) {
        throw_errno(path, errno);
    }
    // Python's open() refuses a directory itself, before any read.
    struct stat info {};
    if (::fstat(file.get(), &info) == 0 && S_ISDIR(info.st_mode)) {
        throw_errno(path, EISDIR);
    }
    std::string bytes;
    char buffer[64 * 1024];
    for (;;) {
        const ssize_t got = ::read(file.get(), buffer, sizeof buffer);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_errno(path, errno);
        }
        if (got == 0) {
            break;
        }
        bytes.append(buffer, static_cast<std::size_t>(got));
    }
    return bytes;
}

} // namespace fv::detail
