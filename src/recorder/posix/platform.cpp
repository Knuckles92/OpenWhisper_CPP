#include "recorder/detail/platform.h"

#include <cerrno>
#include <cstdio>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fv::recorder_detail {

namespace {

[[noreturn]] void throw_errno(int code, const char* what) {
    throw std::system_error(code, std::generic_category(), what);
}

std::intptr_t open_fd(const std::filesystem::path& path, int flags, const char* what) {
    int fd = -1;
    do {
        // 0666 before the umask, as Python's open() creates files.
        fd = ::open(path.c_str(), flags | O_CLOEXEC, 0666);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        throw_errno(errno, what);
    }
    return fd;
}

} // namespace

OsFile::~OsFile() {
    if (handle_ != -1) {
        ::close(static_cast<int>(handle_));
    }
}

OsFile::OsFile(OsFile&& other) noexcept : handle_(std::exchange(other.handle_, -1)) {}

OsFile& OsFile::operator=(OsFile&& other) noexcept {
    if (this != &other) {
        if (handle_ != -1) {
            ::close(static_cast<int>(handle_));
        }
        handle_ = std::exchange(other.handle_, -1);
    }
    return *this;
}

OsFile OsFile::create_new(const std::filesystem::path& path) {
    return OsFile(open_fd(path, O_RDWR | O_CREAT | O_EXCL, "Could not create the file"));
}

OsFile OsFile::open_append(const std::filesystem::path& path) {
    return OsFile(open_fd(path, O_RDWR | O_CREAT | O_APPEND, "Could not open the file"));
}

OsFile OsFile::open_read(const std::filesystem::path& path) {
    return OsFile(open_fd(path, O_RDONLY, "Could not open the file"));
}

OsFile OsFile::create_truncate(const std::filesystem::path& path) {
    return OsFile(open_fd(path, O_RDWR | O_CREAT | O_TRUNC, "Could not create the file"));
}

bool OsFile::is_open() const {
    return handle_ != -1;
}

std::size_t OsFile::write(std::span<const std::byte> data) {
    ssize_t written = -1;
    do {
        written = ::write(static_cast<int>(handle_), data.data(), data.size());
    } while (written < 0 && errno == EINTR);
    if (written < 0) {
        throw_errno(errno, "Could not write the file");
    }
    return static_cast<std::size_t>(written);
}

void OsFile::write_all(std::span<const std::byte> data) {
    while (!data.empty()) {
        const std::size_t written = write(data);
        if (written == 0) {
            throw_errno(ENOSPC, "Could not write the file");
        }
        data = data.subspan(written);
    }
}

std::size_t OsFile::read(std::span<std::byte> out) {
    std::size_t total = 0;
    while (total < out.size()) {
        const ssize_t got =
            ::read(static_cast<int>(handle_), out.data() + total, out.size() - total);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_errno(errno, "Could not read the file");
        }
        if (got == 0) {
            break;
        }
        total += static_cast<std::size_t>(got);
    }
    return total;
}

void OsFile::seek(std::uint64_t offset) {
    if (::lseek(static_cast<int>(handle_), static_cast<off_t>(offset), SEEK_SET) < 0) {
        throw_errno(errno, "Could not seek");
    }
}

std::uint64_t OsFile::size() const {
    struct stat info {};
    if (::fstat(static_cast<int>(handle_), &info) != 0) {
        throw_errno(errno, "Could not read the file size");
    }
    return static_cast<std::uint64_t>(info.st_size);
}

void OsFile::sync() {
    if (::fsync(static_cast<int>(handle_)) != 0) {
        throw_errno(errno, "Could not flush the file");
    }
}

void OsFile::close() {
    if (handle_ != -1) {
        const int fd = static_cast<int>(std::exchange(handle_, -1));
        if (::close(fd) != 0 && errno != EINTR) {
            throw_errno(errno, "Could not close the file");
        }
    }
}

bool OsFile::try_lock() {
    if (::flock(static_cast<int>(handle_), LOCK_EX | LOCK_NB) == 0) {
        return true;
    }
    if (errno == EWOULDBLOCK) {
        return false;
    }
    throw_errno(errno, "Could not lock the file");
}

void replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (std::rename(from.c_str(), to.c_str()) != 0) {
        throw_errno(errno, "Could not replace the file");
    }
}

void set_thread_name(std::string_view name) {
    // Linux limits a thread name to 15 bytes.
    const std::string shortened(name.substr(0, 15));
    pthread_setname_np(pthread_self(), shortened.c_str());
}

} // namespace fv::recorder_detail
