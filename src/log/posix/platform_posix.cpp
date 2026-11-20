#include "log/detail/files.h"

#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <sys/utsname.h>
#include <system_error>
#include <unistd.h>

namespace fv::log::detail {

void write_new_file(const std::filesystem::path& path, std::string_view bytes) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0) {
        throw std::filesystem::filesystem_error("Cannot create the file", path,
                                                std::error_code(errno, std::generic_category()));
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
            throw std::filesystem::filesystem_error("Cannot write the file", path,
                                                    std::error_code(code, std::generic_category()));
        }
        data += written;
        left -= static_cast<std::size_t>(written);
    }
    ::close(fd);
}

void set_thread_name(const char* name) {
    char shortened[16] = {};
    std::size_t i = 0;
    for (; name[i] != '\0' && i < sizeof(shortened) - 1; ++i) {
        shortened[i] = name[i];
    }
    pthread_setname_np(pthread_self(), shortened);
}

void write_stderr(std::string_view utf8) {
    std::size_t left = utf8.size();
    const char* data = utf8.data();
    while (left > 0) {
        const ssize_t written = ::write(STDERR_FILENO, data, left);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        data += written;
        left -= static_cast<std::size_t>(written);
    }
}

std::string os_system() {
    utsname name{};
    if (::uname(&name) != 0) {
        return "";
    }
    return name.sysname;
}

std::string os_release() {
    utsname name{};
    if (::uname(&name) != 0) {
        return "";
    }
    return name.release;
}

std::optional<std::int64_t> cpu_count() {
    const long count = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (count < 1) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(count);
}

} // namespace fv::log::detail
