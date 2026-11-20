#include "settings/detail/disk_file.h"

#include <windows.h>

#include <string>
#include <system_error>

#include "base/utf8.h"
#include "pyport/text.h"

namespace fv::detail {

namespace {

std::int64_t ticks(const FILETIME& time) {
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(time.dwHighDateTime) << 32) |
                                     time.dwLowDateTime);
}

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

// open() on Windows goes through the C runtime's _wopen, so Python reports the errno the CRT
// maps the Win32 error to, never the Win32 code itself.
[[noreturn]] void throw_open_error(const std::filesystem::path& path, DWORD code) {
    const std::string where = py::repr(path_to_utf8(path));
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
        throw SettingsFileError("FileNotFoundError",
                                "[Errno 2] No such file or directory: " + where);
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_CURRENT_DIRECTORY:
    case ERROR_NETWORK_ACCESS_DENIED:
        throw SettingsFileError("PermissionError", "[Errno 13] Permission denied: " + where);
    default:
        throw SettingsFileError("OSError", "[Errno 22] Invalid argument: " + where);
    }
}

} // namespace

std::optional<FileSignature> stat_signature(const std::filesystem::path& absolute) {
    const Handle file(CreateFileW(absolute.c_str(), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!file.valid()) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return FileSignature{.path = absolute};
        }
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info)) {
        return std::nullopt;
    }
    return FileSignature{
        .path = absolute,
        .exists = true,
        .volume = info.dwVolumeSerialNumber,
        .index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
        .size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
        .modified = ticks(info.ftLastWriteTime),
        .changed = ticks(info.ftCreationTime),
    };
}

std::string read_whole_file(const std::filesystem::path& path) {
    const Handle file(CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        throw_open_error(path, GetLastError());
    }
    std::string bytes;
    char buffer[64 * 1024];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file.get(), buffer, sizeof buffer, &got, nullptr)) {
            const DWORD code = GetLastError();
            throw SettingsFileError("OSError",
                                    "[Errno 5] Input/output error: " +
                                        std::system_category().message(static_cast<int>(code)));
        }
        if (got == 0) {
            break;
        }
        bytes.append(buffer, got);
    }
    return bytes;
}

} // namespace fv::detail
