#include "net/lan_address.h"

#include <algorithm>
#include <array>
#include <bit>
#include <exception>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "base/log.h"
#include "net/address.h"
#include "net/detail/asio.h"
#include "pyport/text.h"

#include <boost/asio/ip/host_name.hpp>
#include <boost/asio/ip/udp.hpp>

namespace fv::net {

namespace {

constexpr log::Logger kLog("services.lan_address");

// Interface names, lowercased prefixes.
constexpr std::array<std::string_view, 15> kVpnNames = {
    "tun",      "tap",     "wg", "ppp", "ipsec",   "utun", "gpd", "proton",
    "nordlynx", "mullvad", "zt", "vpn", "cscotun", "ivpn", "pia"};
constexpr std::array<std::string_view, 18> kVirtualNames = {
    "docker", "br-",     "veth", "virbr", "vmnet", "vboxnet", "lxcbr", "lxdbr",  "podman",
    "cni",    "flannel", "cali", "kube",  "awdl",  "llw",     "anpi",  "bridge", "vethernet"};
// Windows adapter descriptions, lowercased substrings.
constexpr std::array<std::string_view, 13> kVpnDescriptions = {
    "tap-windows", "wintun",     "wireguard", "openvpn",  "vpn",   "proton", "nord",
    "mullvad",     "anyconnect", "fortinet",  "zerotier", "pangp", "juniper"};
constexpr std::array<std::string_view, 8> kVirtualDescriptions = {
    "hyper-v", "vmware", "virtualbox", "wsl", "vethernet", "npcap loopback", "docker", "parallels"};

// Windows IF_TYPE_* values (ipifcons.h).
constexpr int kIfTypePpp = 23;
constexpr int kIfTypePropVirtual = 53;
constexpr int kIfTypeTunnel = 131;
constexpr int kIfTypeEthernet = 6;
constexpr int kIfTypeWifi = 71;

bool starts_with_any(std::string_view text, std::span<const std::string_view> prefixes) {
    return std::ranges::any_of(prefixes, [&](std::string_view p) { return text.starts_with(p); });
}

bool contains_any(std::string_view text, std::span<const std::string_view> words) {
    return std::ranges::any_of(words, [&](std::string_view w) { return text.contains(w); });
}

// 100.64.0.0/10, the tailnet's addresses; an address of another version is never in it.
bool in_tailscale_network(const IpAddress& ip) {
    return ip.version == 4 && ip.bytes[0] == 100 && (ip.bytes[1] & 0xC0) == 64;
}

bool private_address(std::string_view address) {
    const auto ip = parse_address(address);
    return ip && is_private(*ip);
}

} // namespace

LocalAddress make_local_address(std::string address, std::string interface_name,
                                std::string_view kind, int prefix, bool physical) {
    LocalAddress entry;
    entry.is_private = private_address(address);
    entry.address = std::move(address);
    entry.interface_name = std::move(interface_name);
    entry.kind = std::string(kind);
    entry.is_lan = kind == kLan;
    entry.is_vpn = kind == kVpn;
    entry.is_virtual = kind == kVirtual;
    entry.is_tailscale = kind == kTailscale;
    entry.prefix = prefix;
    entry.physical = physical;
    return entry;
}

bool usable(std::string_view address) {
    const auto ip = parse_address(address);
    if (!ip || ip->version != 4) {
        return false;
    }
    const bool unspecified =
        ip->bytes[0] == 0 && ip->bytes[1] == 0 && ip->bytes[2] == 0 && ip->bytes[3] == 0;
    const bool multicast = (ip->bytes[0] & 0xF0) == 224;
    return !is_loopback(*ip) && !unspecified && !is_link_local(*ip) && !multicast;
}

int prefix_length(std::span<const std::uint8_t> mask) {
    int bits = 0;
    for (const std::uint8_t byte : mask) {
        bits += std::popcount(static_cast<unsigned>(byte));
    }
    return bits;
}

LocalAddress classify(const LanInterface& interface) {
    const auto sysfs = system_linux_sysfs();
    return classify(interface, sysfs ? &*sysfs : nullptr);
}

LocalAddress classify(const LanInterface& interface, const LinuxSysfs* sysfs) {
    const std::string name = py::lower(interface.name);
    const std::string description = py::lower(interface.description);
    const auto ip = parse_address(interface.address);
    if (!ip) {
        throw std::invalid_argument("'" + interface.address +
                                    "' does not appear to be an IPv4 or IPv6 address");
    }
    bool physical = false;
    std::string_view kind;
    if (in_tailscale_network(*ip) || name.starts_with("tailscale") ||
        description.contains("tailscale")) {
        kind = kTailscale;
    } else if (interface.if_type) {
        // Windows: the adapter's type and description.
        const int type = *interface.if_type;
        if (type == kIfTypePpp || type == kIfTypePropVirtual || type == kIfTypeTunnel ||
            contains_any(description, kVpnDescriptions)) {
            kind = kVpn;
        } else if (contains_any(description, kVirtualDescriptions) ||
                   name.starts_with("vethernet")) {
            kind = kVirtual;
        } else {
            kind = kLan;
            physical = type == kIfTypeEthernet || type == kIfTypeWifi;
        }
    } else {
        std::optional<std::string> linux_kind;
        if (sysfs != nullptr && sysfs->kind) {
            linux_kind = sysfs->kind(interface);
        }
        if (linux_kind) {
            kind = *linux_kind == kVpn ? kVpn : (*linux_kind == kVirtual ? kVirtual : kLan);
        } else if (interface.point_to_point || interface.prefix == 32 ||
                   starts_with_any(name, kVpnNames)) {
            kind = kVpn;
        } else if (starts_with_any(name, kVirtualNames)) {
            kind = kVirtual;
        } else {
            kind = kLan;
        }
        if (kind == kLan && sysfs != nullptr && sysfs->physical) {
            physical = sysfs->physical(interface.name);
        }
    }
    return make_local_address(interface.address, interface.name, kind, interface.prefix, physical);
}

std::tuple<int, bool, bool, bool> rank(const LocalAddress& entry,
                                       const std::optional<std::string>& route) {
    int order = 3;
    if (entry.kind == kLan) {
        order = 0;
    } else if (entry.kind == kVpn) {
        order = 1;
    } else if (entry.kind == kTailscale) {
        order = 2;
    }
    // `entry.address != route`: an address never equals None.
    const bool off_route = !route || entry.address != *route;
    return {order, !entry.is_private, off_route, !entry.physical};
}

std::optional<std::string> probe_route_address() {
    // A UDP connect toward TEST-NET-1 picks the source address of the default route and sends
    // nothing.
    namespace asio = boost::asio;
    try {
        asio::io_context io;
        asio::ip::udp::socket probe(io);
        boost::system::error_code error;
        probe.open(asio::ip::udp::v4(), error);
        if (error) {
            return std::nullopt;
        }
        probe.connect({asio::ip::make_address_v4("192.0.2.1"), 9}, error);
        if (error) {
            return std::nullopt;
        }
        const auto local = probe.local_endpoint(error);
        if (error) {
            return std::nullopt;
        }
        return local.address().to_string();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<std::string> probe_hostname_addresses() {
    namespace asio = boost::asio;
    std::vector<std::string> found;
    try {
        asio::io_context io;
        asio::ip::udp::resolver resolver(io);
        boost::system::error_code error;
        const std::string host = asio::ip::host_name(error);
        if (error) {
            return found;
        }
        const auto results = resolver.resolve(asio::ip::udp::v4(), host, "0",
                                              asio::ip::resolver_base::numeric_service, error);
        if (error) {
            return found;
        }
        for (const auto& result : results) {
            found.push_back(result.endpoint().address().to_string());
        }
    } catch (const std::exception&) {
        found.clear();
    }
    return found;
}

LanProbes system_lan_probes() {
    LanProbes probes;
    probes.interfaces = [] {
        try {
            return system_interfaces();
        } catch (const std::exception& error) {
            kLog.debug("Could not list network interfaces: {}", error.what());
        }
        return std::optional<std::vector<LanInterface>>();
    };
    probes.route_address = probe_route_address;
    probes.hostname_addresses = probe_hostname_addresses;
    probes.linux_sysfs = system_linux_sysfs();
    return probes;
}

std::vector<LocalAddress> local_ipv4_addresses() {
    return local_ipv4_addresses(system_lan_probes());
}

std::vector<LocalAddress> local_ipv4_addresses(const LanProbes& probes) {
    const auto listed =
        probes.interfaces ? probes.interfaces() : std::optional<std::vector<LanInterface>>();
    const auto route = probes.route_address ? probes.route_address() : std::nullopt;
    const LinuxSysfs* sysfs = probes.linux_sysfs ? &*probes.linux_sysfs : nullptr;
    const bool have_listing = listed && !listed->empty();

    std::vector<LocalAddress> addresses;
    std::unordered_set<std::string> seen;
    if (listed) {
        for (const auto& interface : *listed) {
            if (usable(interface.address) && !seen.contains(interface.address)) {
                seen.insert(interface.address);
                addresses.push_back(classify(interface, sysfs));
            }
        }
    }
    // A listing already holds every address the host name could resolve to, and resolving it
    // can block for seconds (mDNS, an unreachable DNS server).
    std::vector<std::optional<std::string>> extra{route};
    if (!have_listing && probes.hostname_addresses) {
        for (auto& address : probes.hostname_addresses()) {
            extra.emplace_back(std::move(address));
        }
    }
    for (const auto& address : extra) {
        if (address && !address->empty() && usable(*address) && !seen.contains(*address)) {
            seen.insert(*address);
            const auto ip = parse_address(*address);
            const bool tailnet = ip && in_tailscale_network(*ip);
            addresses.push_back(make_local_address(*address, "", tailnet ? kTailscale : kLan));
        }
    }
    std::ranges::stable_sort(addresses, [&](const LocalAddress& a, const LocalAddress& b) {
        return rank(a, route) < rank(b, route);
    });
    return addresses;
}

std::optional<LocalAddress> best_lan_address() {
    return best_lan_address(system_lan_probes());
}

std::optional<LocalAddress> best_lan_address(const LanProbes& probes) {
    for (auto& entry : local_ipv4_addresses(probes)) {
        if (entry.kind == kLan || entry.kind == kVpn) {
            return std::move(entry);
        }
    }
    return std::nullopt;
}

std::optional<std::string> discover_lan_ipv4() {
    return discover_lan_ipv4(system_lan_probes());
}

std::optional<std::string> discover_lan_ipv4(const LanProbes& probes) {
    auto best = best_lan_address(probes);
    if (!best) {
        return std::nullopt;
    }
    return std::move(best->address);
}

} // namespace fv::net
