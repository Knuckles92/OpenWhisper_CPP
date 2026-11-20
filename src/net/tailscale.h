#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "net/address.h"

namespace fv::net {

// The pure half of services/remote_asr/tailscale.py: which addresses are Tailscale's, and
// what `tailscale status --json` says. Running the CLI is agentapi/tailscale_probe.h's.

// Addresses Tailscale hands out: 100.64.0.0/10 and fd7a:115c:a1e0::/48. Text goes through
// str(address).split("%", 1)[0] and ip_address, so junk is false; an IPv4-mapped address
// answers for its IPv4 address.
bool is_tailscale_address(std::string_view address);
bool is_tailscale_address(const IpAddress& address);

struct TailscalePeer {
    std::string name;
    std::string dns_name;
    std::string address;
    std::string os;
    bool online = false;
    std::string owner;
    bool tagged = false;
    bool mine = false;

    // A desktop OS as `status --json` spells it (windows, linux, macos), in any case.
    bool is_desktop() const;
};

struct TailscaleStatus {
    // running, stopped, needs_login, starting, not_installed or unavailable.
    std::string state;
    std::string detail;
    std::string name;
    std::string dns_name;
    std::string address;
    std::string owner;
    std::string tailnet;
    std::vector<TailscalePeer> peers; // this owner's computers first, then by name

    bool running() const { return state == "running"; }
};

// Which Tailscale node and user an address belongs to (`tailscale whois --json`).
struct PeerIdentity {
    std::string node;
    std::string owner;
    bool tagged = false;
};

// parse_status over the CLI's JSON object, with Python's truthiness and str() of whatever
// the fields hold. Throws py::TypeError where Python's would (TailscaleIPs a number) and
// std::invalid_argument for a document that is not an object.
TailscaleStatus parse_status(const Json& data);

} // namespace fv::net
