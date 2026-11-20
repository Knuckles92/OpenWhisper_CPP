#include <windows.h>

#include <algorithm>
#include <system_error>
#include <utility>

#include "download/detail/raw_file.h"

namespace fv::download::detail {

namespace {

[[noreturn]] void fail(DWORD code, const char* what) {
    throw std::system_error(static_cast<int>(code), std::system_category(), what);
}

[[noreturn]] void fail_last(const char* what) {
    fail(GetLastError(), what);
}

HANDLE open(const std::filesystem::path& path, DWORD access, DWORD disposition) {
    // The C runtime's open (Python's os.open) shares reads and writes; a reparse point is opened
    // as itself so it can be refused.
    HANDLE handle =
        CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, disposition,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        fail_last("open");
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        const DWORD code = GetLastError();
        CloseHandle(handle);
        fail(code, "open");
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(handle);
        // What O_NOFOLLOW reports for a symbolic link.
        fail(ERROR_CANT_ACCESS_FILE, "open");
    }
    return handle;
}

} // namespace

RawFile RawFile::open_partial(const std::filesystem::path& path) {
    RawFile file;
    file.handle_ = open(path, GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS);
    return file;
}

RawFile RawFile::open_read_nofollow(const std::filesystem::path& path) {
    RawFile file;
    file.handle_ = open(path, GENERIC_READ, OPEN_EXISTING);
    return file;
}

RawFile::~RawFile() {
    close();
}

RawFile::RawFile(RawFile&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

RawFile& RawFile::operator=(RawFile&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

FileInfo RawFile::info() const {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle_, &info)) {
        fail_last("fstat");
    }
    FileInfo out;
    out.regular =
        (info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) == 0 &&
        GetFileType(handle_) == FILE_TYPE_DISK;
    out.links = info.nNumberOfLinks;
    out.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    return out;
}

std::size_t RawFile::read(std::span<char> buffer) {
    std::size_t total = 0;
    while (total < buffer.size()) {
        const auto want =
            static_cast<DWORD>(std::min<std::size_t>(buffer.size() - total, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(handle_, buffer.data() + total, want, &got, nullptr)) {
            fail_last("read");
        }
        if (got == 0) {
            break;
        }
        total += got;
    }
    return total;
}

void RawFile::write(std::span<const char> data) {
    std::size_t total = 0;
    while (total < data.size()) {
        const auto want = static_cast<DWORD>(std::min<std::size_t>(data.size() - total, 1u << 30));
        DWORD wrote = 0;
        if (!WriteFile(handle_, data.data() + total, want, &wrote, nullptr)) {
            fail_last("write");
        }
        total += wrote;
    }
}

void RawFile::seek(std::uint64_t offset) {
    LARGE_INTEGER position;
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) {
        fail_last("seek");
    }
}

void RawFile::truncate(std::uint64_t size) {
    FILE_END_OF_FILE_INFO end{};
    end.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFileInformationByHandle(handle_, FileEndOfFileInfo, &end, sizeof(end))) {
        fail_last("truncate");
    }
}

void RawFile::sync() {
    if (!FlushFileBuffers(handle_)) {
        fail_last("fsync");
    }
}

void RawFile::make_private() {}

void RawFile::close() {
    if (handle_ != nullptr) {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
}

} // namespace fv::download::detail
