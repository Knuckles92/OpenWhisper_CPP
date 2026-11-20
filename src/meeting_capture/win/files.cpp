#include "meeting_capture/detail/files.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <io.h>
#include <share.h>

#include <cerrno>
#include <string>
#include <system_error>

namespace fv::meeting::detail {

std::FILE* open_file(const std::filesystem::path& path, const char* mode) {
    std::wstring wide_mode;
    for (const char* c = mode; *c != '\0'; ++c) {
        wide_mode.push_back(static_cast<wchar_t>(*c));
    }
    // _wfsopen with _SH_DENYNO shares the file as Python's open() does, so a reader of the
    // session PCM never blocks the writer.
    return _wfsopen(path.c_str(), wide_mode.c_str(), _SH_DENYNO);
}

void BinaryFile::sync_descriptor() {
    // os.fsync is _commit on Windows, which refuses a read-only descriptor (EBADF): callers that
    // fsync a finished file reopen it "r+b" first.
    if (_commit(_fileno(file_)) != 0) {
        throw std::system_error(errno, std::generic_category(), "Could not flush the file");
    }
}

void replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Could not replace the file");
    }
}

void set_thread_name(std::string_view name) {
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length > 0 ? length : 0), L'\0');
    if (length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wide.data(),
                            length);
    }
    SetThreadDescription(GetCurrentThread(), wide.c_str());
}

} // namespace fv::meeting::detail
