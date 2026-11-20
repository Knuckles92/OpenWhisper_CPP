#include "audio/detail/raw_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <system_error>

namespace fv::audio::detail {

namespace {

[[noreturn]] void throw_errno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

int as_fd(std::intptr_t value) {
    return static_cast<int>(value);
}

} // namespace

RawFile RawFile::open_existing(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        throw_errno("Could not open the file");
    }
    RawFile file;
    file.handle_ = fd;
    return file;
}

RawFile RawFile::create_temp(const std::filesystem::path& directory, std::string_view suffix,
                             std::filesystem::path& created) {
    // tempfile.TMP_MAX tries before giving up; mode 0600 as mkstemp makes it.
    for (int attempt = 0; attempt < 10000; ++attempt) {
        const std::filesystem::path candidate = directory / random_temp_name(suffix);
        const int fd = ::open(candidate.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            created = candidate;
            RawFile file;
            file.handle_ = fd;
            return file;
        }
        if (errno != EEXIST) {
            throw_errno("Could not create a temporary file");
        }
    }
    throw std::system_error(std::make_error_code(std::errc::file_exists),
                            "No usable temporary file name found");
}

std::uint64_t RawFile::seek_end() {
    const off_t end = ::lseek(as_fd(handle_), 0, SEEK_END);
    if (end < 0) {
        throw_errno("Could not seek");
    }
    return static_cast<std::uint64_t>(end);
}

void RawFile::seek(std::uint64_t offset) {
    if (::lseek(as_fd(handle_), static_cast<off_t>(offset), SEEK_SET) < 0) {
        throw_errno("Could not seek");
    }
}

std::size_t RawFile::write(std::span<const std::byte> data) {
    const ssize_t written = ::write(as_fd(handle_), data.data(), data.size());
    if (written < 0) {
        throw_errno("Could not write");
    }
    return static_cast<std::size_t>(written);
}

void RawFile::truncate(std::uint64_t size) {
    if (::ftruncate(as_fd(handle_), static_cast<off_t>(size)) != 0) {
        throw_errno("Could not truncate");
    }
}

void RawFile::sync() {
    if (::fsync(as_fd(handle_)) != 0) {
        throw_errno("Could not flush the file to disk");
    }
}

void RawFile::close() {
    if (handle_ == -1) {
        return;
    }
    const int fd = as_fd(handle_);
    handle_ = -1;
    if (::close(fd) != 0) {
        throw_errno("Could not close the file");
    }
}

void replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (std::rename(from.c_str(), to.c_str()) != 0) {
        throw_errno("Could not replace the file");
    }
}

} // namespace fv::audio::detail
