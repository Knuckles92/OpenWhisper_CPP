#include "base/detail/lease_file.h"

#include <windows.h>

#include <string>
#include <system_error>

#include "base/data_lease.h"

namespace fv::detail {

namespace {

HANDLE as_handle(std::intptr_t file) {
    return reinterpret_cast<HANDLE>(file);
}

[[noreturn]] void throw_last_error(const char* what, const std::filesystem::path& path) {
    const std::error_code error(static_cast<int>(GetLastError()), std::system_category());
    throw std::filesystem::filesystem_error(what, path, error);
}

} // namespace

std::intptr_t open_lease_file(const std::filesystem::path& path) {
    // The sharing Python's os.open gives (the CRT's _SH_DENYNO): every process opens the file.
    const HANDLE file =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw_last_error("Cannot open the application data lock", path);
    }
    const auto handle = reinterpret_cast<std::intptr_t>(file);
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        CloseHandle(file);
        throw StartupRestoreError("The application data lock is not a file.");
    }
    if (info.nFileSizeHigh == 0 && info.nFileSizeLow == 0) {
        const char zero = '\0';
        DWORD written = 0;
        if (!WriteFile(file, &zero, 1, &written, nullptr)) {
            const DWORD code = GetLastError();
            CloseHandle(file);
            SetLastError(code);
            throw_last_error("Cannot write the application data lock", path);
        }
    }
    return handle;
}

bool lock_lease_file(std::intptr_t file, bool exclusive) {
    OVERLAPPED at{}; // offset 0
    const DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0);
    return LockFileEx(as_handle(file), flags, 0, 1, 0, &at) != FALSE;
}

void unlock_and_close_lease_file(std::intptr_t file) {
    OVERLAPPED at{};
    UnlockFileEx(as_handle(file), 0, 1, 0, &at);
    CloseHandle(as_handle(file));
}

void close_lease_file(std::intptr_t file) {
    CloseHandle(as_handle(file));
}

std::filesystem::path normcase(const std::filesystem::path& path) {
    std::wstring text = path.native();
    for (auto& c : text) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    if (text.empty()) {
        return path;
    }
    // Python's str.lower() does not depend on the locale; neither does the invariant mapping.
    const int size = static_cast<int>(text.size());
    std::wstring lower(text.size(), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), size, lower.data(), size,
                      nullptr, nullptr, 0) != size) {
        return std::filesystem::path(text);
    }
    return std::filesystem::path(lower);
}

} // namespace fv::detail
