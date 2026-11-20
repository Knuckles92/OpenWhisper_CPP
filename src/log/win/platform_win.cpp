#include "log/detail/files.h"

#include <windows.h>

#include <array>
#include <string>
#include <system_error>
#include <tuple>

namespace fv::log::detail {

namespace {

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

struct OsVersion {
    DWORD major = 0;
    DWORD minor = 0;
    DWORD build = 0;
    bool client = true;
};

// RtlGetVersion is not shimmed by the application manifest, unlike GetVersionEx, so it reports
// the version Python's win32_ver finds.
std::optional<OsVersion> os_version() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        return std::nullopt;
    }
    const auto get_version =
        reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!get_version) {
        return std::nullopt;
    }
    OSVERSIONINFOEXW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (get_version(&info) != 0) {
        return std::nullopt;
    }
    return OsVersion{info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber,
                     info.wProductType == VER_NT_WORKSTATION};
}

} // namespace

void write_new_file(const std::filesystem::path& path, std::string_view bytes) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
            throw std::filesystem::filesystem_error("File exists", path,
                                                    std::make_error_code(std::errc::file_exists));
        }
        throw std::filesystem::filesystem_error(
            "Cannot create the file", path,
            std::error_code(static_cast<int>(code), std::system_category()));
    }
    const char* data = bytes.data();
    std::size_t left = bytes.size();
    while (left > 0) {
        const DWORD chunk = static_cast<DWORD>(left < (1u << 30) ? left : (1u << 30));
        DWORD written = 0;
        if (!WriteFile(file, data, chunk, &written, nullptr)) {
            const DWORD code = GetLastError();
            CloseHandle(file);
            throw std::filesystem::filesystem_error(
                "Cannot write the file", path,
                std::error_code(static_cast<int>(code), std::system_category()));
        }
        data += written;
        left -= written;
    }
    CloseHandle(file);
}

void set_thread_name(const char* name) {
    const std::wstring wide = widen(name);
    SetThreadDescription(GetCurrentThread(), wide.c_str());
}

void write_stderr(std::string_view utf8) {
    const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode)) {
        std::wstring wide;
        wide.reserve(utf8.size() + 8);
        for (const wchar_t c : widen(utf8)) {
            if (c == L'\n') {
                wide += L'\r';
            }
            wide += c;
        }
        DWORD written = 0;
        WriteConsoleW(handle, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
        return;
    }
    const std::string bytes = text_mode_bytes(utf8);
    DWORD written = 0;
    WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

std::string os_system() {
    return "Windows";
}

// platform.release(): win32_ver's release from Python 3.12's _WIN32_CLIENT_RELEASES and
// _WIN32_SERVER_RELEASES, the first entry whose version is not above the running one.
std::string os_release() {
    const auto version = os_version();
    if (!version) {
        return "";
    }
    using Entry = std::tuple<DWORD, DWORD, DWORD, const char*>;
    static constexpr std::array<Entry, 11> kClient{{
        {10, 1, 0, "post11"},
        {10, 0, 22000, "11"},
        {6, 4, 0, "10"},
        {6, 3, 0, "8.1"},
        {6, 2, 0, "8"},
        {6, 1, 0, "7"},
        {6, 0, 0, "Vista"},
        {5, 2, 3790, "XP64"},
        {5, 2, 0, "XPMedia"},
        {5, 1, 0, "XP"},
        {5, 0, 0, "2000"},
    }};
    static constexpr std::array<Entry, 11> kServer{{
        {10, 1, 0, "post2025Server"},
        {10, 0, 26100, "2025Server"},
        {10, 0, 20348, "2022Server"},
        {10, 0, 17763, "2019Server"},
        {6, 4, 0, "2016Server"},
        {6, 3, 0, "2012ServerR2"},
        {6, 2, 0, "2012Server"},
        {6, 1, 0, "2008ServerR2"},
        {6, 0, 0, "2008Server"},
        {5, 2, 0, "2003Server"},
        {5, 0, 0, "2000Server"},
    }};
    const auto running = std::make_tuple(version->major, version->minor, version->build);
    for (const auto& [major, minor, build, name] : version->client ? kClient : kServer) {
        if (std::make_tuple(major, minor, build) <= running) {
            return name;
        }
    }
    return "";
}

std::optional<std::int64_t> cpu_count() {
    const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (count == 0) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(count);
}

} // namespace fv::log::detail
