#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fv::net {

// An IPv4 UDP socket for the remote engine's discovery (discovery.py): a responder bound to its
// port, or a searcher that never binds. Blocking, for worker threads. On Windows an ICMP "port
// unreachable" after a send to a closed port no longer fails the next receive
// (SIO_UDP_CONNRESET off), and one that still does reads as no datagram, as Python's loops
// treat it.
// Affinity: one thread at a time, except close(), which may come from any thread.
class UdpSocket {
public:
    // socket(AF_INET, SOCK_DGRAM).bind((address, port)), with no address-reuse option. The error
    // is the OS message as Python's OSError.strerror gives it (no trailing period or line end).
    static std::expected<std::unique_ptr<UdpSocket>, std::string> bind(std::string_view address,
                                                                       std::uint16_t port);
    // SO_BROADCAST and no bind: the first send picks a port, as any client's would, so searching
    // never registers this app as something that listens (and Windows asks nothing about the
    // firewall). Throws std::system_error when the OS gives no socket.
    static std::unique_ptr<UdpSocket> unbound_broadcast();

    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // The bound port; 0 for an unbound socket before its first send.
    std::uint16_t local_port() const;
    // One datagram to address:port (an IPv4 address or a name). False when the OS refused it or
    // the socket is closed.
    bool send_to(std::span<const std::byte> bytes, std::string_view address, std::uint16_t port);

    struct Datagram {
        std::vector<std::byte> bytes;
        std::string sender; // numeric IPv4
        std::uint16_t port = 0;
    };
    // The next datagram, at most 2,048 bytes as discovery.py's recvfrom reads it; nullopt on
    // timeout, an ICMP reset, any other receive error, or once closed.
    std::optional<Datagram> receive(std::chrono::milliseconds timeout);
    // Closes the socket; a receive waiting on another thread returns nullopt at once.
    void close();

private:
    struct Impl;
    explicit UdpSocket(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace fv::net
