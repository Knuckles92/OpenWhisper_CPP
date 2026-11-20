#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The UDP socket discovery uses, behind a seam so that every search and responder test runs over
// an in-memory network (remote/testing/fake_discovery_bus.h) instead of a network card.
namespace fv::remote {

// One datagram received: its bytes and who sent it (address without scope, and port).
struct Datagram {
    std::vector<std::byte> bytes;
    std::string sender;
    std::uint16_t port = 0;
};

// One UDP socket (IPv4, as discovery.py's). A search never binds: its first send picks a port, as
// any client's would, so searching never registers this app as something that listens (no
// firewall prompt). A responder binds DISCOVERY_PORT.
// Affinity: one thread at a time; close() may come from any thread and ends a receive.
class DiscoveryTransport {
public:
    virtual ~DiscoveryTransport() = default;
    // Binds the socket; the port bound (port 0: an ephemeral one), or why it couldn't
    // (the system's text, as Python's OSError.strerror).
    virtual std::expected<std::uint16_t, std::string> bind(std::string_view address,
                                                           std::uint16_t port) = 0;
    // Sends one datagram; false when the system refused it (Python logs and goes on).
    virtual bool send_to(std::span<const std::byte> bytes, std::string_view address,
                         std::uint16_t port) = 0;
    // The next datagram, or nullopt after `timeout` or on an error a receive shrugs off (on
    // Windows, the ICMP "port unreachable" an earlier send provoked) or once closed.
    virtual std::optional<Datagram> receive(std::chrono::milliseconds timeout) = 0;
    virtual void close() = 0;
};

// Makes a socket; `broadcast` sets SO_BROADCAST (a search's).
using DiscoveryTransportFactory =
    std::function<std::unique_ptr<DiscoveryTransport>(bool broadcast)>;

// Real UDP sockets over net::UdpSocket. Declared here; its definition belongs to the client
// package (discovery_transport.cpp), which builds on the socket layer.
DiscoveryTransportFactory udp_discovery();

} // namespace fv::remote
