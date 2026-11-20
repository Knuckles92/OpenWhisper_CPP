#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>
#include <utility>

#include "download/detail/raw_file.h"

namespace fv::download::detail {

namespace {

[[noreturn]] void fail(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

} // namespace

RawFile RawFile::open_partial(const std::filesystem::path& path) {
    RawFile file;
    file.fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file.fd_ < 0) {
        fail("open");
    }
    return file;
}

RawFile RawFile::open_read_nofollow(const std::filesystem::path& path) {
    RawFile file;
    file.fd_ = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (file.fd_ < 0) {
        fail("open");
    }
    return file;
}

RawFile::~RawFile() {
    close();
}

RawFile::RawFile(RawFile&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

RawFile& RawFile::operator=(RawFile&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

FileInfo RawFile::info() const {
    struct stat st {};
    if (::fstat(fd_, &st) != 0) {
        fail("fstat");
    }
    return FileInfo{S_ISREG(st.st_mode), static_cast<std::uint64_t>(st.st_nlink),
                    static_cast<std::uint64_t>(st.st_size)};
}

std::size_t RawFile::read(std::span<char> buffer) {
    std::size_t total = 0;
    while (total < buffer.size()) {
        const auto got = ::read(fd_, buffer.data() + total, buffer.size() - total);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("read");
        }
        if (got == 0) {
            break;
        }
        total += static_cast<std::size_t>(got);
    }
    return total;
}

void RawFile::write(std::span<const char> data) {
    std::size_t total = 0;
    while (total < data.size()) {
        const auto wrote = ::write(fd_, data.data() + total, data.size() - total);
        if (wrote < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("write");
        }
        total += static_cast<std::size_t>(wrote);
    }
}

void RawFile::seek(std::uint64_t offset) {
    if (::lseek(fd_, static_cast<off_t>(offset), SEEK_SET) < 0) {
        fail("seek");
    }
}

void RawFile::truncate(std::uint64_t size) {
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
        fail("truncate");
    }
}

void RawFile::sync() {
    if (::fsync(fd_) != 0) {
        fail("fsync");
    }
}

void RawFile::make_private() {
    if (::fchmod(fd_, 0600) != 0) {
        fail("fchmod");
    }
}

void RawFile::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace fv::download::detail
