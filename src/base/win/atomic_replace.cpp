#include "base/detail/atomic_replace.h"

#include <windows.h>

#include <algorithm>
#include <system_error>

namespace fv::detail {

namespace {

[[noreturn]] void throw_error(const char* what, const std::filesystem::path& path, DWORD code) {
    throw std::filesystem::filesystem_error(
        what, path, std::error_code(static_cast<int>(code), std::system_category()));
}

} // namespace

bool create_temp_file(const std::filesystem::path& temp, std::string_view bytes, bool fsync) {
    const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
            return false;
        }
        throw_error("Cannot create the temporary file", temp, code);
    }
    const char* data = bytes.data();
    std::size_t left = bytes.size();
    while (left > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(left, 1u << 30));
        DWORD written = 0;
        if (!WriteFile(file, data, chunk, &written, nullptr)) {
            const DWORD code = GetLastError();
            CloseHandle(file);
            throw_error("Cannot write the temporary file", temp, code);
        }
        data += written;
        left -= written;
    }
    if (fsync && !FlushFileBuffers(file)) {
        const DWORD code = GetLastError();
        CloseHandle(file);
        throw_error("Cannot flush the temporary file", temp, code);
    }
    if (!CloseHandle(file)) {
        throw_error("Cannot close the temporary file", temp, GetLastError());
    }
    return true;
}

void replace_file(const std::filesystem::path& temp, const std::filesystem::path& target) {
    if (!MoveFileExW(temp.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw_error("Cannot replace the file", target, GetLastError());
    }
}

} // namespace fv::detail
