#include "history/detail/exclusive_file.h"

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

ExclusiveFile::~ExclusiveFile() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

std::unique_ptr<ExclusiveFile> ExclusiveFile::create(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (errno == EEXIST) {
            return nullptr;
        }
        throw_error("Cannot create the recording", path, errno);
    }
    return std::unique_ptr<ExclusiveFile>(new ExclusiveFile(path, fd));
}

void ExclusiveFile::write(std::span<const std::byte> bytes) {
    const auto* data = reinterpret_cast<const char*>(bytes.data());
    std::size_t left = bytes.size();
    while (left > 0) {
        const ssize_t written = ::write(fd_, data, left);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_error("Cannot write the recording", path_, errno);
        }
        data += written;
        left -= static_cast<std::size_t>(written);
    }
}

void ExclusiveFile::sync() {
    if (::fsync(fd_) != 0) {
        throw_error("Cannot flush the recording", path_, errno);
    }
}

void ExclusiveFile::close() {
    if (fd_ >= 0) {
        const int fd = fd_;
        fd_ = -1;
        if (::close(fd) != 0) {
            throw_error("Cannot close the recording", path_, errno);
        }
    }
}

bool same_normalized_path(const std::filesystem::path& a, const std::filesystem::path& b) {
    return std::filesystem::absolute(a).lexically_normal() ==
           std::filesystem::absolute(b).lexically_normal();
}

} // namespace fv::history_detail
