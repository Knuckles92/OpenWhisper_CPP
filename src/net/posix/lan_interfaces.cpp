#include "net/lan_address.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "pyport/number.h"

namespace fv::net {

namespace {

#ifdef __linux__
// ARPHRD_* device types for tunnels: PPP, IPIP, SIT, GRE and "none" (tun and WireGuard).
const std::set<std::int64_t> kArphrdTunnels = {512, 768, 776, 778, 65534};

std::filesystem::path sysfs(const std::string& name, const char* leaf) {
    return std::filesystem::path("/sys/class/net") / name / leaf;
}

std::optional<std::string> linux_kind(const LanInterface& interface) {
    std::ifstream file(sysfs(interface.name, "type"));
    if (!file) {
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    // int(text.strip() or 0)
    std::int64_t type = 0;
    if (text.find_first_not_of(" \t\r\n\f\v") != std::string::npos) {
        const auto parsed = py::py_int(text);
        if (!parsed) {
            return std::nullopt;
        }
        type = *parsed;
    }
    std::error_code ignored;
    if (kArphrdTunnels.contains(type) ||
        std::filesystem::exists(sysfs(interface.name, "tun_flags"), ignored)) {
        return std::string(kVpn);
    }
    return std::nullopt;
}

bool linux_physical(const std::string& name) {
    std::error_code ignored;
    return std::filesystem::exists(sysfs(name, "device"), ignored);
}
#endif

} // namespace

std::optional<std::vector<LanInterface>> system_interfaces() {
    ifaddrs* head = nullptr;
    if (getifaddrs(&head) != 0) {
        return std::nullopt;
    }
    std::vector<LanInterface> found;
    for (const ifaddrs* entry = head; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((entry->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        const auto* address = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
        std::array<char, INET_ADDRSTRLEN> text{};
        if (inet_ntop(AF_INET, &address->sin_addr, text.data(), text.size()) == nullptr) {
            continue;
        }
        LanInterface interface;
        interface.name = entry->ifa_name != nullptr ? entry->ifa_name : "";
        interface.address = text.data();
        if (entry->ifa_netmask != nullptr) {
            const auto* mask = reinterpret_cast<const sockaddr_in*>(entry->ifa_netmask);
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&mask->sin_addr);
            interface.prefix = prefix_length(std::span<const std::uint8_t>(bytes, 4));
        }
        interface.point_to_point = (entry->ifa_flags & IFF_POINTOPOINT) != 0;
        found.push_back(std::move(interface));
    }
    freeifaddrs(head);
    return found;
}

std::optional<LinuxSysfs> system_linux_sysfs() {
#ifdef __linux__
    return LinuxSysfs{linux_kind, linux_physical};
#else
    return std::nullopt;
#endif
}

} // namespace fv::net
