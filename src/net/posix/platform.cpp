#include "net/detail/platform.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>

namespace fv::net::detail {

void set_thread_name(const std::string& name) {
#if defined(__APPLE__)
    pthread_setname_np(name.substr(0, 63).c_str());
#else
    // The kernel's limit is 16 bytes including the terminator; longer names are refused.
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#endif
}

std::error_code set_address_reuse(std::uintptr_t native_socket, bool /*exclusive*/) {
    const int on = 1;
    if (setsockopt(static_cast<int>(native_socket), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) !=
        0) {
        return {errno, std::generic_category()};
    }
    return {};
}

std::expected<std::vector<IpAddress>, std::string> getaddrinfo_stream(const std::string& name) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
#ifdef AI_IDN
    hints.ai_flags = AI_IDN;
#endif
    addrinfo* found = nullptr;
    const int code = getaddrinfo(name.c_str(), nullptr, &hints, &found);
    if (code != 0) {
        return std::unexpected("[Errno " + std::to_string(code) + "] " + gai_strerror(code));
    }
    std::vector<IpAddress> addresses;
    for (const addrinfo* info = found; info != nullptr; info = info->ai_next) {
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
    freeaddrinfo(found);
    return addresses;
}

bool probe_bind(const IpAddress& address) {
    const int family = address.version == 6 ? AF_INET6 : AF_INET;
    const int probe = socket(family, SOCK_STREAM, 0);
    if (probe < 0) {
        return false;
    }
    int result = -1;
    if (address.version == 6) {
        sockaddr_in6 where{};
        where.sin6_family = AF_INET6;
        std::memcpy(&where.sin6_addr, address.bytes.data(), 16);
        unsigned scope = 0;
        const auto* begin = address.scope_id.data();
        const auto parsed = std::from_chars(begin, begin + address.scope_id.size(), scope);
        if (parsed.ec != std::errc() && !address.scope_id.empty()) {
            scope = if_nametoindex(address.scope_id.c_str());
        }
        where.sin6_scope_id = scope;
        result = bind(probe, reinterpret_cast<const sockaddr*>(&where), sizeof(where));
    } else {
        sockaddr_in where{};
        where.sin_family = AF_INET;
        std::memcpy(&where.sin_addr, address.bytes.data(), 4);
        result = bind(probe, reinterpret_cast<const sockaddr*>(&where), sizeof(where));
    }
    close(probe);
    return result == 0;
}

} // namespace fv::net::detail
