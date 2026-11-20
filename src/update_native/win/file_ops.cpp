#include "update_native/detail/file_ops.h"

#include <windows.h>

namespace fv::update_native::detail {
namespace fs = std::filesystem;

bool unsafe_node(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        throw fs::filesystem_error("Reading update file attributes", path,
                                   {static_cast<int>(GetLastError()), std::system_category()});
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

void flush_file(const fs::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw fs::filesystem_error("Opening update file for flush", path,
                                   {static_cast<int>(GetLastError()), std::system_category()});
    }
    const bool flushed = FlushFileBuffers(file) != 0;
    const auto error = GetLastError();
    CloseHandle(file);
    if (!flushed) {
        throw fs::filesystem_error("Flushing update file", path,
                                   {static_cast<int>(error), std::system_category()});
    }
}

// The Win32 directory commit is performed by MOVEFILE_WRITE_THROUGH at publication.
void flush_directory(const fs::path&) {}

void replace_file(const fs::path& source, const fs::path& destination) {
    if (!MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING)) {
        throw fs::filesystem_error("Restoring update snapshot", source, destination,
                                   {static_cast<int>(GetLastError()), std::system_category()});
    }
}

void publish_new(const fs::path& source, const fs::path& destination) {
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        throw fs::filesystem_error("Publishing a verified update", source, destination,
                                   {static_cast<int>(GetLastError()), std::system_category()});
    }
}
} // namespace fv::update_native::detail
