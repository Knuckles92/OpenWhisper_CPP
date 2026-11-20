#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace fv::net {

// This computer's address on the local network, told apart from a VPN's (services/
// lan_address.py). The route probe answers with whichever interface holds the default route,
// which with a VPN up is the tunnel, so the interfaces are listed and each address classified:
// "lan" (an ordinary network card), "vpn" (a tunnel), "virtual" (Docker, VM and WSL bridges,
// reachable only from this computer) and "tailscale" (the tailnet address). Only classify()
// throws, as Python's does, for an address that is not an IP address.

inline constexpr std::string_view kLan = "lan";
inline constexpr std::string_view kVpn = "vpn";
inline constexpr std::string_view kVirtual = "virtual";
inline constexpr std::string_view kTailscale = "tailscale";

// One classified IPv4 address. The flags repeat `kind`, set together by classify().
struct LocalAddress {
    std::string address;
    std::string interface_name; // Python's `interface`; empty for a probed address
    std::string kind = std::string(kLan);
    bool is_lan = true;
    bool is_vpn = false;
    bool is_virtual = false;
    bool is_tailscale = false;
    int prefix = 0;
    // A real network card (Linux: backed by a device; Windows: Ethernet or Wi-Fi).
    bool physical = false;
    // ipaddress.ip_address(address).is_private
    bool is_private = false;

    bool operator==(const LocalAddress&) const = default;
};

// LocalAddress(address, interface, kind, prefix, physical) with the flags filled in.
LocalAddress make_local_address(std::string address, std::string interface_name,
                                std::string_view kind, int prefix = 0, bool physical = false);

// One address of one interface, as the platform lists it (Python's _Interface).
struct LanInterface {
    std::string name;
    std::string address;
    int prefix = 0;
    bool point_to_point = false;
    std::string description;
    std::optional<int> if_type; // Windows IfType; nullopt elsewhere
};

// What Linux's /sys/class/net says. Set on Linux only: elsewhere classify() never asks.
struct LinuxSysfs {
    // "vpn" for a tunnel device, nullopt when sysfs says nothing.
    std::function<std::optional<std::string>(const LanInterface&)> kind;
    // Backed by a device (/sys/class/net/<name>/device exists).
    std::function<bool(const std::string& name)> physical;
};

// Where local_ipv4_addresses() looks; tests replace these as the Python tests patch the
// module's functions.
struct LanProbes {
    // Every IPv4 address with its interface; nullopt when the listing failed or the platform
    // has none. An empty listing counts as none, as Python's `if listed` does.
    std::function<std::optional<std::vector<LanInterface>>()> interfaces;
    // The address holding the default route (a UDP connect that sends nothing).
    std::function<std::optional<std::string>()> route_address;
    // The host name's IPv4 addresses. Blocking (DNS, mDNS); asked only without a listing.
    std::function<std::vector<std::string>()> hostname_addresses;
    std::optional<LinuxSysfs> linux_sysfs;
};

// This computer's probes.
LanProbes system_lan_probes();
// GetAdaptersAddresses on Windows (adapters up, not loopback), getifaddrs elsewhere.
std::optional<std::vector<LanInterface>> system_interfaces();
// The real sysfs on Linux, nullopt elsewhere.
std::optional<LinuxSysfs> system_linux_sysfs();
std::optional<std::string> probe_route_address();
std::vector<std::string> probe_hostname_addresses();

// _usable: an IPv4 address that is not loopback, unspecified, link-local or multicast.
bool usable(std::string_view address);
// _prefix_length: the set bits of a netmask.
int prefix_length(std::span<const std::uint8_t> mask);

// Labels one interface address as LAN, VPN, virtual or Tailscale. The first form asks this
// computer's sysfs on Linux. Throws std::invalid_argument when `address` is not an IP address
// (Python's ValueError).
LocalAddress classify(const LanInterface&);
LocalAddress classify(const LanInterface&, const LinuxSysfs* sysfs);

// The sort key of local_ipv4_addresses: kind (lan, vpn, tailscale, virtual), private first,
// the default route's address first, physical first.
std::tuple<int, bool, bool, bool> rank(const LocalAddress&,
                                       const std::optional<std::string>& route);

// This computer's usable IPv4 addresses, classified, best LAN first. The route probe adds an
// address the listing missed; without a listing the host name's addresses are added too. Both
// are judged by address alone. Blocking: never on the GUI thread or the reactor.
std::vector<LocalAddress> local_ipv4_addresses();
std::vector<LocalAddress> local_ipv4_addresses(const LanProbes&);

// Where other computers here can reach this one: a LAN address, else a VPN's (check `kind`
// before offering it as a LAN one). Never a Docker or VM bridge, nor the Tailscale address.
std::optional<LocalAddress> best_lan_address();
std::optional<LocalAddress> best_lan_address(const LanProbes&);

// best_lan_address as a plain string, for links other devices open.
std::optional<std::string> discover_lan_ipv4();
std::optional<std::string> discover_lan_ipv4(const LanProbes&);

} // namespace fv::net
