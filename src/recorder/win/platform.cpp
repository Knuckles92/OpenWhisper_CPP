#include "recorder/detail/platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <limits>
#include <system_error>
#include <utility>

namespace fv::recorder_detail {

namespace {

HANDLE as_handle(std::intptr_t value) {
    return reinterpret_cast<HANDLE>(value);
}

[[noreturn]] void throw_error(DWORD code, const char* what) {
    throw std::system_error(static_cast<int>(code), std::system_category(), what);
}

[[noreturn]] void throw_last_error(const char* what) {
    throw_error(GetLastError(), what);
}

std::intptr_t open_handle(const std::filesystem::path& path, DWORD access, DWORD disposition,
                          const char* what) {
    // Python's open() shares reading and writing; deleting is shared too so a reader never
    // keeps a finished session from being removed.
    const HANDLE handle =
        CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw_last_error(what);
    }
    return reinterpret_cast<std::intptr_t>(handle);
}

} // namespace

OsFile::~OsFile() {
    if (handle_ != -1) {
        CloseHandle(as_handle(handle_));
    }
}

OsFile::OsFile(OsFile&& other) noexcept : handle_(std::exchange(other.handle_, -1)) {}

OsFile& OsFile::operator=(OsFile&& other) noexcept {
    if (this != &other) {
        if (handle_ != -1) {
            CloseHandle(as_handle(handle_));
        }
        handle_ = std::exchange(other.handle_, -1);
    }
    return *this;
}

OsFile OsFile::create_new(const std::filesystem::path& path) {
    return OsFile(
        open_handle(path, GENERIC_READ | GENERIC_WRITE, CREATE_NEW, "Could not create the file"));
}

OsFile OsFile::open_append(const std::filesystem::path& path) {
    OsFile file(
        open_handle(path, GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS, "Could not open the file"));
    LARGE_INTEGER end{};
    if (!SetFilePointerEx(as_handle(file.handle_), LARGE_INTEGER{}, &end, FILE_END)) {
        throw_last_error("Could not seek");
    }
    return file;
}

OsFile OsFile::open_read(const std::filesystem::path& path) {
    return OsFile(open_handle(path, GENERIC_READ, OPEN_EXISTING, "Could not open the file"));
}

OsFile OsFile::create_truncate(const std::filesystem::path& path) {
    return OsFile(open_handle(path, GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS,
                              "Could not create the file"));
}

bool OsFile::is_open() const {
    return handle_ != -1;
}

std::size_t OsFile::write(std::span<const std::byte> data) {
    const auto chunk =
        static_cast<DWORD>(std::min<std::size_t>(data.size(), std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(as_handle(handle_), data.data(), chunk, &written, nullptr)) {
        throw_last_error("Could not write the file");
    }
    return written;
}

void OsFile::write_all(std::span<const std::byte> data) {
    while (!data.empty()) {
        const std::size_t written = write(data);
        if (written == 0) {
            throw_error(ERROR_DISK_FULL, "Could not write the file");
        }
        data = data.subspan(written);
    }
}

std::size_t OsFile::read(std::span<std::byte> out) {
    std::size_t total = 0;
    while (total < out.size()) {
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(out.size() - total, std::numeric_limits<DWORD>::max()));
        DWORD got = 0;
        if (!ReadFile(as_handle(handle_), out.data() + total, chunk, &got, nullptr)) {
            throw_last_error("Could not read the file");
        }
        if (got == 0) {
            break;
        }
        total += got;
    }
    return total;
}

void OsFile::seek(std::uint64_t offset) {
    LARGE_INTEGER target{};
    target.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(as_handle(handle_), target, nullptr, FILE_BEGIN)) {
        throw_last_error("Could not seek");
    }
}

std::uint64_t OsFile::size() const {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(as_handle(handle_), &size)) {
        throw_last_error("Could not read the file size");
    }
    return static_cast<std::uint64_t>(size.QuadPart);
}

void OsFile::sync() {
    if (!FlushFileBuffers(as_handle(handle_))) {
        throw_last_error("Could not flush the file");
    }
}

void OsFile::close() {
    if (handle_ != -1) {
        const HANDLE handle = as_handle(std::exchange(handle_, -1));
        if (!CloseHandle(handle)) {
            throw_last_error("Could not close the file");
        }
    }
}

bool OsFile::try_lock() {
    OVERLAPPED at{}; // byte 0
    if (LockFileEx(as_handle(handle_), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                   &at)) {
        return true;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING) {
        return false;
    }
    throw_error(error, "Could not lock the file");
}

void replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (!MoveFileExW(from.c_str(), to.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw_last_error("Could not replace the file");
    }
}

void set_thread_name(std::string_view name) {
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    if (length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wide.data(),
                            length);
    }
    SetThreadDescription(GetCurrentThread(), wide.c_str());
}

} // namespace fv::recorder_detail
