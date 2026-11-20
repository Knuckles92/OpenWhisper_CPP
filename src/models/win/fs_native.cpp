#include <windows.h>

#include <cerrno>
#include <string>
#include <vector>

#include "models/detail/fs_native.h"

namespace fv::models::detail {

namespace {

// FileLock's open retries while the lock file is pending deletion or briefly held without
// sharing (an indexer or a scanner): up to 2 s.
constexpr int kOpenAttempts = 100;
constexpr DWORD kOpenRetryMs = 20;

// 100 ns ticks between 1601-01-01 and 1970-01-01.
constexpr std::int64_t kEpochTicks = 116444736000000000LL;

std::int64_t to_unix_ns(const FILETIME& time) {
    const auto ticks = (static_cast<std::int64_t>(time.dwHighDateTime) << 32) |
                       static_cast<std::int64_t>(time.dwLowDateTime);
    return (ticks - kEpochTicks) * 100;
}

} // namespace

std::optional<FileIdentity> file_identity(const std::filesystem::path& path) {
    // FILE_FLAG_BACKUP_SEMANTICS opens directories too.
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = GetFileInformationByHandle(file, &info);
    CloseHandle(file);
    if (!ok) {
        return std::nullopt;
    }
    FileIdentity identity;
    identity.volume = info.dwVolumeSerialNumber;
    identity.index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    identity.mtime_ns = to_unix_ns(info.ftLastWriteTime);
    identity.size = (static_cast<std::int64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    return identity;
}

std::int64_t walk_size_bytes(const std::filesystem::path& directory) {
    std::int64_t total = 0;
    std::vector<std::filesystem::path> pending{directory};
    while (!pending.empty()) {
        const std::filesystem::path folder = std::move(pending.back());
        pending.pop_back();
        WIN32_FIND_DATAW data{};
        // The listing carries every size, so no file is opened (an lstat per file cost seconds
        // on trees of tens of thousands of files).
        HANDLE find = FindFirstFileExW((folder / L"*").c_str(), FindExInfoBasic, &data,
                                       FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            continue;
        }
        do {
            const std::wstring_view name(data.cFileName);
            if (name == L"." || name == L"..") {
                continue;
            }
            const bool symlink = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
                                 data.dwReserved0 == IO_REPARSE_TAG_SYMLINK;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && !symlink) {
                pending.push_back(folder / std::wstring(name));
            } else {
                total += (static_cast<std::int64_t>(data.nFileSizeHigh) << 32) |
                         static_cast<std::int64_t>(data.nFileSizeLow);
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    return total;
}

int python_winerror(const std::error_code& error) {
    return error.category() == std::system_category() ? error.value() : 0;
}

int python_errno(const std::error_code& error) {
    if (error.category() != std::system_category()) {
        return error.value();
    }
    // CPython's winerror_to_errno (PC/errmap.h), for the errors a file operation meets.
    const int code = error.value();
    if (code == ERROR_ACCESS_DENIED || code == ERROR_CURRENT_DIRECTORY ||
        (code >= ERROR_WRITE_PROTECT && code <= ERROR_SHARING_BUFFER_EXCEEDED) ||
        code == ERROR_NETWORK_ACCESS_DENIED || code == ERROR_CANNOT_MAKE ||
        code == ERROR_FAIL_I24 || code == ERROR_DRIVE_LOCKED || code == ERROR_SEEK_ON_DEVICE ||
        code == ERROR_NOT_LOCKED || code == ERROR_LOCK_FAILED) {
        return EACCES;
    }
    switch (code) {
    case ERROR_DISK_FULL:
        return ENOSPC;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_NO_MORE_FILES:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_FILENAME_EXCED_RANGE:
        return ENOENT;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return EEXIST;
    case ERROR_NOT_SAME_DEVICE:
        return EXDEV;
    case ERROR_DIR_NOT_EMPTY:
        return ENOTEMPTY;
    case ERROR_TOO_MANY_OPEN_FILES:
        return EMFILE;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_NOT_ENOUGH_QUOTA:
        return ENOMEM;
    default:
        return EINVAL;
    }
}

std::uint64_t current_pid() {
    return GetCurrentProcessId();
}

std::string text_mode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '\n') {
            out.push_back('\r');
        }
        out.push_back(c);
    }
    return out;
}

// filelock's WindowsFileLock removes the lock file on release. The file is opened without
// FILE_SHARE_DELETE, so the removal after the close fails, harmlessly, while another holder or
// waiter has it open: a waiter never locks a file whose name a newcomer can already create again
// (DeleteFileW unlinks the name at once on NTFS since Windows 10 1903).
FileLock::FileLock(const std::filesystem::path& path) : path_(path) {
    HANDLE file = INVALID_HANDLE_VALUE;
    // A file another program (an indexer or a scanner) held open with FILE_SHARE_DELETE when the
    // last holder removed it is pending deletion, and opening it fails with ERROR_ACCESS_DENIED
    // until that program lets go.
    for (int attempt = 0;; ++attempt) {
        file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            break;
        }
        const DWORD error = GetLastError();
        if ((error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) ||
            attempt >= kOpenAttempts) {
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "could not open the lock file");
        }
        Sleep(kOpenRetryMs);
    }
    OVERLAPPED where{};
    if (!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &where)) {
        const auto code = static_cast<int>(GetLastError());
        CloseHandle(file);
        throw std::system_error(code, std::system_category(), "could not lock the lock file");
    }
    handle_ = reinterpret_cast<std::intptr_t>(file);
}

FileLock::~FileLock() {
    if (handle_ != -1) {
        HANDLE file = reinterpret_cast<HANDLE>(handle_);
        OVERLAPPED where{};
        UnlockFileEx(file, 0, 1, 0, &where);
        CloseHandle(file);
        // Fails while anyone else has the file open; the last one out removes it.
        DeleteFileW(path_.c_str());
    }
}

} // namespace fv::models::detail
