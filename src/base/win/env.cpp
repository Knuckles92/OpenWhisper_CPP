#include "base/env.h"

#include <stdlib.h>

#include <memory>

#include "base/detail/win_text.h"

namespace fv {

namespace {

class ProcessEnv final : public EnvReader {
public:
    std::optional<std::string> get(std::string_view name) const override {
        const std::wstring wide_name = detail::widen(name);
        if (wide_name.empty() || wide_name.find(L'\0') != std::wstring::npos) {
            return std::nullopt;
        }
        // _wdupenv_s reads the same CRT copy as _wgetenv, without its shared static buffer.
        wchar_t* raw = nullptr;
        std::size_t size = 0;
        if (_wdupenv_s(&raw, &size, wide_name.c_str()) != 0 || raw == nullptr) {
            return std::nullopt;
        }
        const std::unique_ptr<wchar_t, decltype(&free)> owned(raw, &free);
        return detail::narrow(std::wstring_view(raw));
    }

    bool is_process() const override { return true; }
};

} // namespace

const EnvReader& system_env() {
    static const ProcessEnv env;
    return env;
}

} // namespace fv
