#include "process/process_env.h"

#include <cstdlib>
#include <stdexcept>

#include "process/win/wide.h"
#include "pyport/text.h"

namespace fv {

namespace {

// os.environ on Windows: the C runtime's environment, names upper-cased. Writes go through
// _wputenv_s, which updates the C runtime's copy and the process environment block, as Python's
// putenv does (an empty value removes the variable there).
class WindowsEnv final : public MutableEnv {
public:
    std::optional<std::string> get(std::string_view name) const override {
        if (name.empty() || name.find('\0') != std::string_view::npos) {
            return std::nullopt;
        }
        const std::wstring wide = process_win::widen(name);
        std::wstring value(256, L'\0');
        for (;;) {
            SetLastError(ERROR_SUCCESS);
            const DWORD size = GetEnvironmentVariableW(wide.c_str(), value.data(),
                                                       static_cast<DWORD>(value.size()));
            if (size == 0) {
                if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
                    return std::nullopt;
                }
                return std::string();
            }
            if (size < value.size()) {
                value.resize(size);
                return process_win::narrow(value);
            }
            value.assign(size, L'\0');
        }
    }

    void set(std::string_view name, std::string_view value) override {
        check_env_assignment(name, value, true);
        if (_wputenv_s(process_win::widen(name).c_str(), process_win::widen(value).c_str()) != 0) {
            throw std::invalid_argument("illegal environment variable name");
        }
    }

    void unset(std::string_view name) override {
        if (name.empty() || name.find('\0') != std::string_view::npos) {
            return;
        }
        _wputenv_s(process_win::widen(name).c_str(), L"");
    }

    std::vector<std::pair<std::string, std::string>> items() const override {
        std::vector<std::pair<std::string, std::string>> out;
        wchar_t* block = GetEnvironmentStringsW();
        if (block == nullptr) {
            return out;
        }
        for (const wchar_t* entry = block; *entry != L'\0'; entry += wcslen(entry) + 1) {
            const std::wstring_view text(entry);
            // The per-drive working folders ("=C:=C:\\") are not variables.
            if (text.front() == L'=') {
                continue;
            }
            const auto equals = text.find(L'=');
            if (equals == std::wstring_view::npos) {
                continue;
            }
            out.emplace_back(py::upper(process_win::narrow(text.substr(0, equals))),
                             process_win::narrow(text.substr(equals + 1)));
        }
        FreeEnvironmentStringsW(block);
        return out;
    }
};

} // namespace

MutableEnv& system_mutable_env() {
    static auto* env = new WindowsEnv();
    return *env;
}

} // namespace fv
