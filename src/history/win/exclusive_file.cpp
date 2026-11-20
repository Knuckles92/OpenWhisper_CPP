#include "history/detail/exclusive_file.h"

#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

#include <cerrno>
#include <cwctype>
#include <string>
#include <system_error>

namespace fv::history_detail {

namespace {

[[noreturn]] void throw_error(const char* what, const std::filesystem::path& path, int code) {
    throw std::filesystem::filesystem_error(what, path,
                                            std::error_code(code, std::generic_category()));
}

// ntpath.normcase: forward slashes as backslashes, then lower case.
std::wstring normcase(const std::filesystem::path& path) {
    std::wstring text = std::filesystem::absolute(path).lexically_normal().native();
    for (wchar_t& c : text) {
        c = c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(c));
    }
    return text;
}

} // namespace

ExclusiveFile::~ExclusiveFile() {
    if (fd_ >= 0) {
        ::_close(fd_);
    }
}

std::unique_ptr<ExclusiveFile> ExclusiveFile::create(const std::filesystem::path& path) {
    int fd = -1;
    // _S_IREAD | _S_IWRITE is what mode 0600 means on Windows: not read-only.
    const errno_t error =
        ::_wsopen_s(&fd, path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT,
                    _SH_DENYNO, _S_IREAD | _S_IWRITE);
    if (error != 0) {
        if (error == EEXIST) {
            return nullptr;
        }
        throw_error("Cannot create the recording", path, error);
    }
    return std::unique_ptr<ExclusiveFile>(new ExclusiveFile(path, fd));
}

void ExclusiveFile::write(std::span<const std::byte> bytes) {
    const auto* data = reinterpret_cast<const char*>(bytes.data());
    std::size_t left = bytes.size();
    while (left > 0) {
        const unsigned chunk = left > (1u << 30) ? (1u << 30) : static_cast<unsigned>(left);
        const int written = ::_write(fd_, data, chunk);
        if (written < 0) {
            throw_error("Cannot write the recording", path_, errno);
        }
        data += written;
        left -= static_cast<std::size_t>(written);
    }
}

void ExclusiveFile::sync() {
    if (::_commit(fd_) != 0) {
        throw_error("Cannot flush the recording", path_, errno);
    }
}

void ExclusiveFile::close() {
    if (fd_ >= 0) {
        const int fd = fd_;
        fd_ = -1;
        if (::_close(fd) != 0) {
            throw_error("Cannot close the recording", path_, errno);
        }
    }
}

bool same_normalized_path(const std::filesystem::path& a, const std::filesystem::path& b) {
    return normcase(a) == normcase(b);
}

} // namespace fv::history_detail
