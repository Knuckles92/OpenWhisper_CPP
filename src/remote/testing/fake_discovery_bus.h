#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "remote/discovery_transport.h"

namespace fv::remote::testing {

// An in-memory UDP network for discovery tests: computers are named by their address, a
// transport belongs to one of them, and a datagram to (address, port) reaches the socket that
// computer has bound to that port. A broadcast address reaches every computer listed for it, and
// 255.255.255.255 every computer on the bus; only a socket made with `broadcast` may send to one
// (as SO_BROADCAST). An unbound socket gets an ephemeral port on its first send. Datagrams can be
// dropped by a filter or delivered after a delay, and every send is recorded.
// Affinity: thread-safe; each transport is used by one thread at a time, as a socket is.
class FakeDiscoveryBus {
public:
    // One send, as the bus saw it.
    struct Sent {
        std::string from;
        std::uint16_t from_port = 0;
        std::string to;
        std::uint16_t port = 0;
        std::vector<std::byte> bytes;
        bool delivered = false; // reached at least one bound socket
    };
    // true drops the datagram on its way from `from` to `to`:`port`.
    using DropFilter = std::function<bool(const Sent& datagram, std::string_view to_host)>;

    static constexpr std::string_view ALL_HOSTS = "255.255.255.255";

    FakeDiscoveryBus();
    FakeDiscoveryBus(const FakeDiscoveryBus&) = delete;
    FakeDiscoveryBus& operator=(const FakeDiscoveryBus&) = delete;
    ~FakeDiscoveryBus();

    // A socket on the computer at `host`.
    std::unique_ptr<DiscoveryTransport> transport(std::string host, bool broadcast = false);
    // Sockets on `host` for code that takes a factory; the bus must outlive what it makes.
    DiscoveryTransportFactory factory(std::string host);

    // Datagrams to `address` reach each computer in `members` (and every computer is known to
    // the bus once it has a transport).
    void add_broadcast(std::string address, std::vector<std::string> members);
    void set_drop(DropFilter filter);
    void set_delay(std::chrono::milliseconds delay);
    // The next bind of `port` on `host` fails with `error` (the system's text).
    void fail_bind(std::string host, std::uint16_t port, std::string error);

    std::vector<Sent> sent() const;
    // Sockets bound now, as "host:port".
    std::vector<std::string> bound() const;

    struct State;

private:
    std::shared_ptr<State> state_;
};

} // namespace fv::remote::testing
