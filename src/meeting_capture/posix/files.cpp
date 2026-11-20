#include "meeting_capture/detail/files.h"

#include <cerrno>
#include <cstdio>
#include <string>
#include <system_error>

#include <pthread.h>
#include <unistd.h>

namespace fv::meeting::detail {

std::FILE* open_file(const std::filesystem::path& path, const char* mode) {
    return std::fopen(path.c_str(), mode);
}

void BinaryFile::sync_descriptor() {
    if (::fsync(::fileno(file_)) != 0) {
        throw std::system_error(errno, std::generic_category(), "Could not flush the file");
    }
}

void replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (std::rename(from.c_str(), to.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(), "Could not replace the file");
    }
}

void set_thread_name(std::string_view name) {
    // Linux limits a thread name to 15 bytes.
    const std::string shortened(name.substr(0, 15));
    pthread_setname_np(pthread_self(), shortened.c_str());
}

} // namespace fv::meeting::detail
