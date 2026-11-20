#include "base/detail/win_text.h"

#include <windows.h>

namespace fv::detail {

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

std::string narrow(std::wstring_view utf16) {
    if (utf16.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), text.data(), size,
                        nullptr, nullptr);
    return text;
}

} // namespace fv::detail
