#include "backup/detail/fs.h"

#include <windows.h>

#include <algorithm>
#include <system_error>

namespace fv::backup::detail {

namespace {

class Handle {
public:
    explicit Handle(HANDLE handle) : handle_(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (valid()) {
            CloseHandle(handle_);
        }
    }
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return handle_; }

private:
    HANDLE handle_;
};

[[noreturn]] void throw_last_error(std::string_view what, const std::filesystem::path& path) {
    const DWORD code = GetLastError();
    throw std::filesystem::filesystem_error(
        std::string(what), path, std::error_code(static_cast<int>(code), std::system_category()));
}

FileStat stat_of(HANDLE handle, const std::filesystem::path& path) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        throw_last_error("stat", path);
    }
    FileStat result;
    result.device = info.dwVolumeSerialNumber;
    result.inode = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    result.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    result.modified = static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
        info.ftLastWriteTime.dwLowDateTime);
    // os.stat gives S_IFDIR to a directory, S_IFCHR or S_IFIFO to a device or pipe, and S_IFREG
    // to everything else.
    result.regular = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                     GetFileType(handle) == FILE_TYPE_DISK;
    return result;
}

} // namespace

bool is_link(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        return false;
    }
    const Handle file(
        CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file.valid()) {
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &tag, sizeof(tag))) {
        return false;
    }
    // is_symlink() sees IO_REPARSE_TAG_SYMLINK and is_junction() IO_REPARSE_TAG_MOUNT_POINT;
    // other reparse points (cloud placeholders, deduplicated files) are ordinary files to Python.
    return tag.ReparseTag == IO_REPARSE_TAG_SYMLINK || tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT;
}

FileStat stat(const std::filesystem::path& path) {
    const Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!file.valid()) {
        throw_last_error("stat", path);
    }
    return stat_of(file.get(), path);
}

void fsync_file(const std::filesystem::path& path) {
    const Handle file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        throw_last_error("open", path);
    }
    if (!FlushFileBuffers(file.get())) {
        throw_last_error("fsync", path);
    }
}

void fsync_directory(const std::filesystem::path&) {}

struct InputFile::Impl {
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::filesystem::path path;
};

InputFile::InputFile(const std::filesystem::path& path) : impl_(std::make_unique<Impl>()) {
    impl_->path = path;
    // The sharing of the C runtime's _wopen, which open(path, "rb") goes through.
    impl_->handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl_->handle == INVALID_HANDLE_VALUE) {
        throw_last_error("open", path);
    }
}

InputFile::~InputFile() {
    if (impl_ && impl_->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->handle);
    }
}

FileStat InputFile::stat() const {
    return stat_of(impl_->handle, impl_->path);
}

std::size_t InputFile::read(std::span<std::byte> buffer) {
    std::size_t total = 0;
    while (total < buffer.size()) {
        DWORD got = 0;
        const auto want =
            static_cast<DWORD>(std::min<std::size_t>(buffer.size() - total, 1u << 30));
        if (!ReadFile(impl_->handle, buffer.data() + total, want, &got, nullptr)) {
            throw_last_error("read", impl_->path);
        }
        if (got == 0) {
            break;
        }
        total += got;
    }
    return total;
}

} // namespace fv::backup::detail
