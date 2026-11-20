#include "remote/detail/normcase.h"

#include <windows.h>

#include <filesystem>
#include <string>

#include "base/utf8.h"

namespace fv::remote::detail {

std::string normcase(std::string_view path) {
    std::string slashed(path);
    for (char& ch : slashed) {
        if (ch == '/') {
            ch = '\\';
        }
    }
    const std::wstring wide = path_from_utf8(slashed).native();
    if (wide.empty()) {
        return slashed;
    }
    const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.c_str(),
                                   static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr, 0);
    if (size <= 0) {
        return slashed;
    }
    std::wstring lowered(static_cast<std::size_t>(size), L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.c_str(),
                  static_cast<int>(wide.size()), lowered.data(), size, nullptr, nullptr, 0);
    return path_to_utf8(std::filesystem::path(lowered));
}

} // namespace fv::remote::detail
