#include "history/detail/export_file.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace fv::history_detail {

namespace {

[[noreturn]] void throw_error(const char* what, const std::filesystem::path& path, int code) {
    throw std::filesystem::filesystem_error(what, path,
                                            std::error_code(code, std::generic_category()));
}

} // namespace

bool create_export_file(const std::filesystem::path& path, std::string_view bytes) {
    // Python's open() creates with 0666 less the umask.
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0) {
        if (errno == EEXIST) {
            return false;
        }
        throw_error("Cannot create the export file", path, errno);
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
            throw_error("Cannot write the export file", path, code);
        }
        data += written;
        left -= static_cast<std::size_t>(written);
    }
    if (::close(fd) != 0) {
        throw_error("Cannot write the export file", path, errno);
    }
    return true;
}

} // namespace fv::history_detail
