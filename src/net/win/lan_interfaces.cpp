#include "net/lan_address.h"

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
// clang-format on

#include <array>
#include <cstring>
#include <vector>

namespace fv::net {

namespace {

std::string narrow(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int length = static_cast<int>(std::wcslen(text));
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), size, nullptr, nullptr);
    return out;
}

} // namespace

std::optional<std::vector<LanInterface>> system_interfaces() {
    // Anycast, multicast and DNS server addresses are not interface addresses.
    constexpr ULONG kFlags =
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 16 * 1024;
    std::vector<std::byte> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    // The list can grow between the size query and the call; Python retries the same way.
    for (int attempt = 0; attempt < 4; ++attempt) {
        buffer.assign(size, std::byte{0});
        result =
            GetAdaptersAddresses(AF_INET, kFlags, nullptr,
                                 reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
        if (result != ERROR_BUFFER_OVERFLOW) {
            break;
        }
    }
    if (result != NO_ERROR) {
        return std::nullopt;
    }
    std::vector<LanInterface> found;
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            const sockaddr* where = unicast->Address.lpSockaddr;
            if (where == nullptr || where->sa_family != AF_INET) {
                continue;
            }
            const auto* v4 = reinterpret_cast<const sockaddr_in*>(where);
            std::array<char, INET_ADDRSTRLEN> text{};
            if (inet_ntop(AF_INET, &v4->sin_addr, text.data(), text.size()) == nullptr) {
                continue;
            }
            LanInterface entry;
            entry.name = narrow(adapter->FriendlyName);
            entry.address = text.data();
            entry.prefix = static_cast<int>(unicast->OnLinkPrefixLength);
            entry.description = narrow(adapter->Description);
            entry.if_type = static_cast<int>(adapter->IfType);
            found.push_back(std::move(entry));
        }
    }
    return found;
}

std::optional<LinuxSysfs> system_linux_sysfs() {
    return std::nullopt;
}

} // namespace fv::net
