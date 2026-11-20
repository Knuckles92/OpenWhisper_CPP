#include "remote/discovery_transport.h"

#include <condition_variable>
#include <mutex>
#include <utility>

#include "net/udp.h"

namespace fv::remote {

namespace {

// One net::UdpSocket behind the seam. A search's socket (broadcast) exists from the start and is
// never bound; a responder's exists once bound. Before then a send fails and a receive finds
// nothing, as a socket that never was would.
// Affinity: one thread at a time; close() from any thread.
class UdpDiscoveryTransport final : public DiscoveryTransport {
public:
    explicit UdpDiscoveryTransport(bool broadcast) {
        if (broadcast) {
            socket_ = net::UdpSocket::unbound_broadcast();
        }
    }

    std::expected<std::uint16_t, std::string> bind(std::string_view address,
                                                   std::uint16_t port) override {
        auto bound = net::UdpSocket::bind(address, port);
        if (!bound) {
            return std::unexpected(bound.error());
        }
        std::shared_ptr<net::UdpSocket> replaced;
        std::lock_guard lock(mutex_);
        if (closed_) {
            (*bound)->close();
            return std::unexpected(std::string("the socket is closed"));
        }
        replaced = std::move(socket_);
        socket_ = std::move(*bound);
        if (replaced) {
            replaced->close();
        }
        closed_changed_.notify_all();
        return socket_->local_port();
    }

    bool send_to(std::span<const std::byte> bytes, std::string_view address,
                 std::uint16_t port) override {
        const auto socket = current();
        return socket && socket->send_to(bytes, address, port);
    }

    std::optional<Datagram> receive(std::chrono::milliseconds timeout) override {
        const auto socket = current();
        if (!socket) {
            // Nothing can arrive on a socket that was never bound; waiting out the timeout keeps
            // a receive loop from spinning. close() ends the wait.
            std::unique_lock lock(mutex_);
            closed_changed_.wait_for(lock, timeout, [&] { return closed_ || socket_ != nullptr; });
            return std::nullopt;
        }
        auto received = socket->receive(timeout);
        if (!received) {
            return std::nullopt;
        }
        return Datagram{std::move(received->bytes), std::move(received->sender), received->port};
    }

    void close() override {
        std::shared_ptr<net::UdpSocket> socket;
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            socket = socket_;
        }
        closed_changed_.notify_all();
        if (socket) {
            socket->close();
        }
    }

private:
    std::shared_ptr<net::UdpSocket> current() const {
        std::lock_guard lock(mutex_);
        return socket_;
    }

    mutable std::mutex mutex_;
    std::condition_variable closed_changed_;
    std::shared_ptr<net::UdpSocket> socket_;
    bool closed_ = false;
};

} // namespace

DiscoveryTransportFactory udp_discovery() {
    return [](bool broadcast) -> std::unique_ptr<DiscoveryTransport> {
        return std::make_unique<UdpDiscoveryTransport>(broadcast);
    };
}

} // namespace fv::remote
