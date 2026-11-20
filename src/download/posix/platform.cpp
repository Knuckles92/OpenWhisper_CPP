#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>

#include "download/detail/platform.h"

namespace fv::download::detail {

std::string system_message(int code) {
    return std::strerror(code);
}

void sync_directory(const std::filesystem::path& directory) {
    const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "open");
    }
    const int result = ::fsync(fd);
    const int error = errno;
    ::close(fd);
    if (result != 0) {
        throw std::system_error(error, std::generic_category(), "fsync");
    }
}

} // namespace fv::download::detail
