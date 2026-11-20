#include "agents/detail/long_path.h"

#include <string>
#include <system_error>

#include <windows.h>

namespace fv::agents::detail {

std::filesystem::path long_real_path(const std::filesystem::path& folder) {
    std::error_code error;
    auto real = std::filesystem::canonical(folder, error);
    if (error) {
        real = std::filesystem::weakly_canonical(std::filesystem::absolute(folder, error), error);
        if (error) {
            real = folder;
        }
    }
    const std::wstring& native = real.native();
    const DWORD size = GetLongPathNameW(native.c_str(), nullptr, 0);
    if (size == 0) {
        return real;
    }
    std::wstring long_form(size, L'\0');
    const DWORD written = GetLongPathNameW(native.c_str(), long_form.data(), size);
    if (written == 0 || written >= size) {
        return real;
    }
    long_form.resize(written);
    return std::filesystem::path(long_form);
}

} // namespace fv::agents::detail
