#include <windows.h>

#include <string>

#include "download/detail/platform.h"

namespace fv::download::detail {

std::string system_message(int code) {
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
    while (!message.empty() && (message.back() <= L' ' || message.back() == L'.')) {
        message.pop_back();
    }
    if (message.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), out.data(),
                        size, nullptr, nullptr);
    return out;
}

void sync_directory(const std::filesystem::path&) {}

} // namespace fv::download::detail
