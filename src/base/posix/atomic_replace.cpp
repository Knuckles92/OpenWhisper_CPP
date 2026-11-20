#include "base/detail/atomic_replace.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <system_error>

namespace fv::detail {

namespace {

[[noreturn]] void throw_error(const char* what, const std::filesystem::path& path, int code) {
    throw std::filesystem::filesystem_error(what, path,
                                            std::error_code(code, std::generic_category()));
}

} // namespace

bool create_temp_file(const std::filesystem::path& temp, std::string_view bytes, bool fsync) {
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (errno == EEXIST) {
            return false;
        }
        throw_error("Cannot create the temporary file", temp, errno);
    }
    const char* data = bytes.data();
    std::size_t left = bytes.size();
    while (left > 0) {
        const ssize_t written = ::write(fd, data, left);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int code = errno;
            ::close(fd);
            throw_error("Cannot write the temporary file", temp, code);
        }
        data += written;
        left -= static_cast<std::size_t>(written);
    }
    if (fsync && ::fsync(fd) != 0) {
        const int code = errno;
        ::close(fd);
        throw_error("Cannot flush the temporary file", temp, code);
    }
    if (::close(fd) != 0) {
        throw_error("Cannot close the temporary file", temp, errno);
    }
    return true;
}

void replace_file(const std::filesystem::path& temp, const std::filesystem::path& target) {
    if (std::rename(temp.c_str(), target.c_str()) != 0) {
        throw_error("Cannot replace the file", target, errno);
    }
}

} // namespace fv::detail
