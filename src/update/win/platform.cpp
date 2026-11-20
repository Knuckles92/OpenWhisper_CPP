#include "update/detail/platform.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "base/utf8.h"

namespace fv::update::detail {

namespace {

class RegistryKey {
public:
    RegistryKey() = default;
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
    ~RegistryKey() {
        if (key_ != nullptr) {
            RegCloseKey(key_);
        }
    }
    HKEY* out() { return &key_; }
    HKEY get() const { return key_; }

private:
    HKEY key_ = nullptr;
};

std::string utf8(std::wstring_view text) {
    return path_to_utf8(std::filesystem::path(text));
}

// winreg.QueryValueEx then str(): text values as they are, a DWORD or QWORD as its decimal
// digits; "" for an absent value or one of another type.
std::string query_value(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) {
        return {};
    }
    std::vector<std::uint8_t> data(size + sizeof(wchar_t));
    if (RegQueryValueExW(key, name, nullptr, &type, data.data(), &size) != ERROR_SUCCESS) {
        return {};
    }
    switch (type) {
    case REG_SZ:
    case REG_EXPAND_SZ: {
        std::wstring text(reinterpret_cast<const wchar_t*>(data.data()), size / sizeof(wchar_t));
        // winreg drops the terminator and anything after the first NUL.
        if (const auto end = text.find(L'\0'); end != std::wstring::npos) {
            text.resize(end);
        }
        return utf8(text);
    }
    case REG_DWORD:
        if (size >= sizeof(DWORD)) {
            DWORD value = 0;
            std::memcpy(&value, data.data(), sizeof(value));
            return std::to_string(value);
        }
        return {};
    case REG_QWORD:
        if (size >= sizeof(std::uint64_t)) {
            std::uint64_t value = 0;
            std::memcpy(&value, data.data(), sizeof(value));
            return std::to_string(value);
        }
        return {};
    default:
        return {};
    }
}

} // namespace

std::optional<UninstallValues> read_uninstall_key(RegistryHive hive, std::string_view key_path,
                                                  int view) {
    const HKEY root = hive == RegistryHive::LocalMachine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    const std::wstring path = path_from_utf8(key_path).wstring();
    RegistryKey key;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ | static_cast<REGSAM>(view), key.out()) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }
    UninstallValues values;
    values.install_location = query_value(key.get(), L"InstallLocation");
    values.uninstall_string = query_value(key.get(), L"UninstallString");
    values.display_version = query_value(key.get(), L"DisplayVersion");
    values.estimated_size = query_value(key.get(), L"EstimatedSize");
    values.install_date = query_value(key.get(), L"InstallDate");
    return values;
}

std::vector<int> registry_views() {
    return {0, KEY_WOW64_64KEY, KEY_WOW64_32KEY};
}

std::filesystem::path executable_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::optional<std::string> file_identity(const std::filesystem::path& path) {
    const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
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
    const std::uint64_t index =
        (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    const std::uint64_t size =
        (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    // FILETIME counts 100 ns since 1601; st_mtime_ns counts ns since 1970.
    constexpr std::int64_t kEpochDelta = 116444736000000000LL;
    const std::int64_t ticks = static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
        info.ftLastWriteTime.dwLowDateTime);
    const std::int64_t mtime_ns = (ticks - kEpochDelta) * 100;
    return std::to_string(index) + ":" + std::to_string(size) + ":" + std::to_string(mtime_ns);
}

} // namespace fv::update::detail
