#include "net/detail/udp_reset.h"

// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
// clang-format on

#include <cstdio>
#include <string>

namespace fv::net::detail {

void disable_udp_connreset(std::uintptr_t native_socket) {
    BOOL report = FALSE;
    DWORD returned = 0;
    // Best effort: without it an ICMP reset still reads as no datagram.
    WSAIoctl(static_cast<SOCKET>(native_socket), SIO_UDP_CONNRESET, &report, sizeof(report),
             nullptr, 0, &returned, nullptr, nullptr);
}

std::string os_error_text(int code) {
    // CPython's PyErr_SetExcFromWindowsErr: the system's message in the neutral language, with
    // trailing whitespace and periods removed.
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring wide;
    if (length > 0 && buffer != nullptr) {
        wide.assign(buffer, length);
    }
    LocalFree(buffer);
    while (!wide.empty() && (wide.back() <= L' ' || wide.back() == L'.')) {
        wide.pop_back();
    }
    if (wide.empty()) {
        return "Windows Error 0x" + [code] {
            char hex[16];
            std::snprintf(hex, sizeof(hex), "%x", static_cast<unsigned>(code));
            return std::string(hex);
        }();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size,
                        nullptr, nullptr);
    return text;
}

} // namespace fv::net::detail
