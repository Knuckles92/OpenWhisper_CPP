#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/cancel.h"

namespace fv::net {

// An IPv4Address or IPv6Address of Python 3.12's ipaddress module.
struct IpAddress {
    int version = 4;                      // 4 or 6
    std::array<std::uint8_t, 16> bytes{}; // network order; IPv4 uses the first 4
    std::string scope_id;                 // IPv6 only ("fe80::1%3"); empty when none

    // str(address): dotted quad, or IPv6 compressed as Python writes it, with "%scope".
    std::string to_string() const;
    // Python's ==: the version, the bits and the scope id.
    bool operator==(const IpAddress&) const = default;
};

// ipaddress.ip_address(text): IPv4 first, then IPv6, by Python's rules (no leading zeros in an
// octet, one "::", an embedded IPv4 tail, "%scope"). nullopt where Python raises ValueError.
std::optional<IpAddress> parse_address(std::string_view text);

// The ipaddress properties, as Python 3.12.10 defines them. An IPv4-mapped IPv6 address answers
// for its IPv4 address, as Python's do.
bool is_loopback(const IpAddress&);
bool is_private(const IpAddress&);
bool is_link_local(const IpAddress&);
bool is_global(const IpAddress&); // not private, and IPv4 not in 100.64.0.0/10

// text.split("%", 1)[0]: the address without its scope, before parsing a peer.
std::string_view strip_scope(std::string_view text);
// address.ipv4_mapped or address: "::ffff:127.0.0.1" as 127.0.0.1, anything else unchanged.
IpAddress unmap_v4(const IpAddress&);

// One getaddrinfo of `name` for TCP (any family), as socket.getaddrinfo(name, port,
// type=SOCK_STREAM), in its order, each address without its scope. The error is the system's
// reason; a cancelled token ends the wait with "cancelled" while the lookup finishes unseen.
// Blocking: never on the GUI thread or the reactor.
std::expected<std::vector<IpAddress>, std::string> resolve_host(std::string_view name,
                                                                const CancelToken&);

// Whether a TCP socket can bind (address, 0): true only for an address of one of this
// computer's interfaces.
bool can_bind(const IpAddress&);

} // namespace fv::net
