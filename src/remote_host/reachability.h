#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/lan_address.h"
#include "remote/control.h"

// Why other computers might not reach this one while it shares its engine
// (services/remote_asr/reachability.py): what this computer can see for itself, each with a plain
// fix. Windows treating the network as Public (the rule Windows offers when sharing starts
// usually covers private networks only), a VPN that may block the local network, and discovery
// that couldn't listen.
// Affinity: thread-safe (pure functions); windows_network_profiles blocks.
namespace fv::remote_host {

// Python's PUBLIC, PRIVATE and DOMAIN: the C runtime's math.h defines DOMAIN as a macro.
inline constexpr std::string_view CATEGORY_PUBLIC = "public";
inline constexpr std::string_view CATEGORY_PRIVATE = "private";
inline constexpr std::string_view CATEGORY_DOMAIN = "domain";
// How long read profiles are used before they are read again, in seconds.
inline constexpr double NETWORK_PROFILES_MAX_AGE_S = 60.0;

// A connected Windows network: its name, adapter, and category (CATEGORY_PUBLIC,
// CATEGORY_PRIVATE or CATEGORY_DOMAIN).
struct NetworkProfile {
    std::string name;
    std::string interface_name;
    std::string category;

    friend bool operator==(const NetworkProfile&, const NetworkProfile&) = default;
};

// Get-NetConnectionProfile | ConvertTo-Json: a list or a single object, NetworkCategory as a
// number (PowerShell 5) or a name; [] for empty text or text that isn't JSON. Entries of another
// category are left out.
std::vector<NetworkProfile> parse_profiles(std::string_view text);

// The connected networks and whether Windows treats each as public, read through PowerShell
// (about 0.3 s); [] elsewhere and when it can't be read.
std::vector<NetworkProfile> windows_network_profiles(double timeout_s = 8.0);

// _settings_page: the Settings page for an adapter ("ms-settings:network-wifi", ...).
std::string settings_page(std::string_view interface_name);

// host_notes: what may keep computers on this network out, most likely first. `lan` is the
// address this host shows (nullopt without one), `addresses` every local IPv4 address,
// `profiles` the Windows network profiles (nullopt while they haven't been read),
// `discovery_error` why discovery isn't answering.
std::vector<remote::Note> host_notes(const std::optional<net::LocalAddress>& lan,
                                     const std::vector<net::LocalAddress>& addresses,
                                     const std::optional<std::vector<NetworkProfile>>& profiles,
                                     std::string_view discovery_error = "");

} // namespace fv::remote_host
