#include "base/detail/lease_file.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

#include "base/data_lease.h"

namespace fv::detail {

namespace {

[[noreturn]] void throw_errno(const char* what, const std::filesystem::path& path, int code) {
    throw std::filesystem::filesystem_error(what, path,
                                            std::error_code(code, std::generic_category()));
}

} // namespace

std::intptr_t open_lease_file(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        throw_errno("Cannot open the application data lock", path, errno);
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(fd);
        throw StartupRestoreError("The application data lock is not a file.");
    }
    if (info.st_size == 0) {
        const char zero = '\0';
        if (::write(fd, &zero, 1) != 1) {
            const int code = errno;
            ::close(fd);
            throw_errno("Cannot write the application data lock", path, code);
        }
    }
    return fd;
}

bool lock_lease_file(std::intptr_t file, bool exclusive) {
    return ::flock(static_cast<int>(file), (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) == 0;
}

void unlock_and_close_lease_file(std::intptr_t file) {
    ::flock(static_cast<int>(file), LOCK_UN);
    ::close(static_cast<int>(file));
}

void close_lease_file(std::intptr_t file) {
    ::close(static_cast<int>(file));
}

std::filesystem::path normcase(const std::filesystem::path& path) {
    return path;
}

} // namespace fv::detail
