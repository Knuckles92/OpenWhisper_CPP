#include <windows.h>

#include <cstring>
#include <cwctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "http/detail/os_text.h"
#include "http/proxy.h"

namespace fv::http {

namespace {

std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

class RegistryKey {
public:
    explicit RegistryKey(const wchar_t* path) {
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key_) != ERROR_SUCCESS) {
            key_ = nullptr;
        }
    }
    ~RegistryKey() {
        if (key_ != nullptr) {
            RegCloseKey(key_);
        }
    }
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    bool open() const { return key_ != nullptr; }

    // The value as Python's winreg returns it, made truthy or a string by the caller: DWORD and
    // QWORD as numbers, strings as text, anything else as raw bytes.
    struct Value {
        DWORD type = REG_NONE;
        std::vector<BYTE> data;
    };
    std::optional<Value> query(const wchar_t* name) const {
        DWORD type = 0;
        DWORD size = 0;
        if (RegQueryValueExW(key_, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        Value value{type, std::vector<BYTE>(size)};
        if (size > 0 && RegQueryValueExW(key_, name, nullptr, &type, value.data.data(), &size) !=
                            ERROR_SUCCESS) {
            return std::nullopt;
        }
        value.data.resize(size);
        return value;
    }

private:
    HKEY key_ = nullptr;
};

bool truthy(const RegistryKey::Value& value) {
    if (value.type == REG_DWORD || value.type == REG_QWORD) {
        for (const BYTE b : value.data) {
            if (b != 0) {
                return true;
            }
        }
        return false;
    }
    if (value.type == REG_SZ || value.type == REG_EXPAND_SZ) {
        return value.data.size() >= sizeof(wchar_t) &&
               reinterpret_cast<const wchar_t*>(value.data.data())[0] != L'\0';
    }
    return !value.data.empty();
}

std::optional<std::string> text(const RegistryKey::Value& value) {
    if (value.type == REG_SZ || value.type == REG_EXPAND_SZ) {
        std::wstring_view wide(reinterpret_cast<const wchar_t*>(value.data.data()),
                               value.data.size() / sizeof(wchar_t));
        if (const auto nul = wide.find(L'\0'); nul != std::wstring_view::npos) {
            wide = wide.substr(0, nul);
        }
        return narrow(wide);
    }
    if (value.type == REG_DWORD && value.data.size() >= sizeof(DWORD)) {
        DWORD number = 0;
        std::memcpy(&number, value.data.data(), sizeof(number));
        return std::to_string(number);
    }
    // str() of bytes or of another kind is not a usable proxy list; the Python's parse of it
    // fails and yields nothing.
    return std::nullopt;
}

} // namespace

namespace detail {

std::vector<std::pair<std::string, std::string>> environment_entries() {
    std::vector<std::pair<std::string, std::string>> entries;
    wchar_t* block = GetEnvironmentStringsW();
    if (block == nullptr) {
        return entries;
    }
    for (const wchar_t* entry = block; *entry != L'\0';) {
        const std::wstring_view line(entry);
        // A name may itself start with "=" (the per-drive current directories).
        const auto equals = line.find(L'=', 1);
        if (equals != std::wstring_view::npos) {
            std::wstring name(line.substr(0, equals));
            for (auto& c : name) {
                c = static_cast<wchar_t>(std::towupper(c));
            }
            entries.emplace_back(narrow(name), narrow(line.substr(equals + 1)));
        }
        entry += line.size() + 1;
    }
    FreeEnvironmentStringsW(block);
    return entries;
}

std::optional<RegistryProxy> read_registry_proxy() {
    const RegistryKey key(LR"(Software\Microsoft\Windows\CurrentVersion\Internet Settings)");
    if (!key.open()) {
        return std::nullopt;
    }
    const auto enable = key.query(L"ProxyEnable");
    if (!enable) {
        return std::nullopt;
    }
    RegistryProxy proxy;
    proxy.enabled = truthy(*enable);
    if (const auto server = key.query(L"ProxyServer")) {
        proxy.server = text(*server);
    }
    if (const auto override_list = key.query(L"ProxyOverride")) {
        proxy.override_list = text(*override_list);
    }
    return proxy;
}

std::string socket_error_text(long code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message;
    if (length > 0 && buffer != nullptr) {
        message.assign(buffer, length);
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    // CPython trims trailing white space and dots from the system's text.
    while (!message.empty() && (message.back() <= L' ' || message.back() == L'.')) {
        message.pop_back();
    }
    return "[WinError " + std::to_string(code) + "] " + narrow(message);
}

std::string dns_failure_text() {
    return "[Errno 11001] getaddrinfo failed";
}

} // namespace detail

} // namespace fv::http
