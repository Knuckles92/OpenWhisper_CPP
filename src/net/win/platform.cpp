#include "net/detail/platform.h"

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// clang-format on

#include <algorithm>
#include <charconv>
#include <cstring>

namespace fv::net::detail {

namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

// Winsock needs WSAStartup before the first call; asio does it for the sockets it opens, but
// these calls may come first. It is never undone: sockets live until exit.
bool winsock_ready() {
    static const bool ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}

std::string winsock_message(int code) {
    return std::system_category().message(code);
}

} // namespace

void set_thread_name(const std::string& name) {
    const std::wstring wide = widen(name);
    SetThreadDescription(GetCurrentThread(), wide.c_str());
}

std::error_code set_address_reuse(std::uintptr_t native_socket, bool exclusive) {
    if (!exclusive) {
        return {};
    }
    const BOOL on = TRUE;
    if (setsockopt(static_cast<SOCKET>(native_socket), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&on), sizeof(on)) == SOCKET_ERROR) {
        return {WSAGetLastError(), std::system_category()};
    }
    return {};
}

std::expected<std::vector<IpAddress>, std::string> getaddrinfo_stream(const std::string& name) {
    if (!winsock_ready()) {
        return std::unexpected(std::string("Windows Sockets could not start"));
    }
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOW* found = nullptr;
    const std::wstring wide = widen(name);
    const int code = GetAddrInfoW(wide.c_str(), nullptr, &hints, &found);
    if (code != 0) {
        return std::unexpected("[Errno " + std::to_string(code) + "] " + winsock_message(code));
    }
    std::vector<IpAddress> addresses;
    for (const ADDRINFOW* info = found; info != nullptr; info = info->ai_next) {
        IpAddress address;
        if (info->ai_family == AF_INET && info->ai_addr != nullptr) {
            const auto* v4 = reinterpret_cast<const sockaddr_in*>(info->ai_addr);
            std::memcpy(address.bytes.data(), &v4->sin_addr, 4);
        } else if (info->ai_family == AF_INET6 && info->ai_addr != nullptr) {
            const auto* v6 = reinterpret_cast<const sockaddr_in6*>(info->ai_addr);
            address.version = 6;
            std::memcpy(address.bytes.data(), &v6->sin6_addr, 16);
        } else {
            continue;
        }
        addresses.push_back(address);
    }
    FreeAddrInfoW(found);
    return addresses;
}

bool probe_bind(const IpAddress& address) {
    if (!winsock_ready()) {
        return false;
    }
    const int family = address.version == 6 ? AF_INET6 : AF_INET;
    const SOCKET probe = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (probe == INVALID_SOCKET) {
        return false;
    }
    int result = SOCKET_ERROR;
    if (address.version == 6) {
        sockaddr_in6 where{};
        where.sin6_family = AF_INET6;
        std::memcpy(&where.sin6_addr, address.bytes.data(), 16);
        unsigned long scope = 0;
        const auto* begin = address.scope_id.data();
        std::from_chars(begin, begin + address.scope_id.size(), scope);
        where.sin6_scope_id = scope;
        result = bind(probe, reinterpret_cast<const sockaddr*>(&where), sizeof(where));
    } else {
        sockaddr_in where{};
        where.sin_family = AF_INET;
        std::memcpy(&where.sin_addr, address.bytes.data(), 4);
        result = bind(probe, reinterpret_cast<const sockaddr*>(&where), sizeof(where));
    }
    closesocket(probe);
    return result == 0;
}

} // namespace fv::net::detail
