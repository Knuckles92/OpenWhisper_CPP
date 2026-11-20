#include "audio/detail/raw_file.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <system_error>

namespace fv::audio::detail {

namespace {

HANDLE as_handle(std::intptr_t value) {
    return reinterpret_cast<HANDLE>(value);
}

[[noreturn]] void throw_last_error(const char* what) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), what);
}

HANDLE open_handle(const std::filesystem::path& path, DWORD disposition) {
    return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

} // namespace

RawFile RawFile::open_existing(const std::filesystem::path& path) {
    const HANDLE handle = open_handle(path, OPEN_EXISTING);
    if (handle == INVALID_HANDLE_VALUE) {
        throw_last_error("Could not open the file");
    }
    RawFile file;
    file.handle_ = reinterpret_cast<std::intptr_t>(handle);
    return file;
}

RawFile RawFile::create_temp(const std::filesystem::path& directory, std::string_view suffix,
                             std::filesystem::path& created) {
    // tempfile.TMP_MAX tries before giving up.
    for (int attempt = 0; attempt < 10000; ++attempt) {
        const std::filesystem::path candidate = directory / random_temp_name(suffix);
        const HANDLE handle = open_handle(candidate, CREATE_NEW);
        if (handle != INVALID_HANDLE_VALUE) {
            created = candidate;
            RawFile file;
            file.handle_ = reinterpret_cast<std::intptr_t>(handle);
            return file;
        }
        if (GetLastError() != ERROR_FILE_EXISTS) {
            throw_last_error("Could not create a temporary file");
        }
    }
    throw std::system_error(std::make_error_code(std::errc::file_exists),
                            "No usable temporary file name found");
}

std::uint64_t RawFile::seek_end() {
    LARGE_INTEGER position{};
    if (!SetFilePointerEx(as_handle(handle_), LARGE_INTEGER{}, &position, FILE_END)) {
        throw_last_error("Could not seek");
    }
    return static_cast<std::uint64_t>(position.QuadPart);
}

void RawFile::seek(std::uint64_t offset) {
    LARGE_INTEGER target{};
    target.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(as_handle(handle_), target, nullptr, FILE_BEGIN)) {
        throw_last_error("Could not seek");
    }
}

std::size_t RawFile::write(std::span<const std::byte> data) {
    // WriteFile takes a 32-bit count.
    const auto count = static_cast<DWORD>(std::min<std::size_t>(data.size(), 1u << 30));
    DWORD written = 0;
    if (!WriteFile(as_handle(handle_), data.data(), count, &written, nullptr)) {
        throw_last_error("Could not write");
    }
    return written;
}

void RawFile::truncate(std::uint64_t size) {
    LARGE_INTEGER position{};
    if (!SetFilePointerEx(as_handle(handle_), LARGE_INTEGER{}, &position, FILE_CURRENT)) {
        throw_last_error("Could not seek");
    }
    seek(size);
    if (!SetEndOfFile(as_handle(handle_))) {
        throw_last_error("Could not truncate");
    }
    // ftruncate leaves the position where it was.
    seek(static_cast<std::uint64_t>(position.QuadPart));
}

void RawFile::sync() {
    if (!FlushFileBuffers(as_handle(handle_))) {
        throw_last_error("Could not flush the file to disk");
    }
}

void RawFile::close() {
    if (handle_ == -1) {
        return;
    }
    const HANDLE handle = as_handle(handle_);
    handle_ = -1;
    if (!CloseHandle(handle)) {
        throw_last_error("Could not close the file");
    }
}

void replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        throw_last_error("Could not replace the file");
    }
}

} // namespace fv::audio::detail
