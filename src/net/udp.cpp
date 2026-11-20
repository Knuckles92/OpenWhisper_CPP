#include "net/udp.h"

#include <array>
#include <atomic>
#include <functional>
#include <system_error>
#include <utility>

// First: it sets the Windows version Asio is built for.
#include "net/detail/asio.h"

#include <boost/asio/ip/udp.hpp>

#include "net/detail/udp_reset.h"

namespace fv::net {

namespace {

namespace asio = boost::asio;
using udp = boost::asio::ip::udp;

// discovery.py's _DATAGRAM_MAX_BYTES: what one recvfrom reads.
constexpr std::size_t kDatagramMax = 2048;

std::string error_text(const boost::system::error_code& error) {
    if (error.category() == boost::system::system_category()) {
        return detail::os_error_text(error.value());
    }
    std::string text = error.message();
    while (!text.empty() &&
           (static_cast<unsigned char>(text.back()) <= ' ' || text.back() == '.')) {
        text.pop_back();
    }
    return text;
}

// (address, port) as Python's AF_INET socket takes it: an IPv4 address, else the first IPv4
// address the name resolves to.
std::optional<udp::endpoint> v4_endpoint(asio::io_context& io, std::string_view address,
                                         std::uint16_t port, boost::system::error_code& error) {
    const auto parsed = asio::ip::make_address_v4(std::string(address), error);
    if (!error) {
        return udp::endpoint(parsed, port);
    }
    udp::resolver resolver(io);
    const auto results =
        resolver.resolve(udp::v4(), std::string(address), std::to_string(port), error);
    if (error || results.empty()) {
        if (!error) {
            error = asio::error::host_not_found;
        }
        return std::nullopt;
    }
    return results.begin()->endpoint();
}

} // namespace

struct UdpSocket::Impl {
    asio::io_context io;
    udp::socket socket{io};
    std::atomic<bool> closed{false};
    std::array<std::byte, kDatagramMax> buffer{};

    // Runs handlers until `done` or the deadline; then cancels and lets the handler run, since
    // it refers to the caller's locals.
    void run_until(const std::function<bool()>& done, std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        io.restart();
        while (!done()) {
            const auto left = deadline - std::chrono::steady_clock::now();
            if (left <= std::chrono::steady_clock::duration::zero() || io.stopped()) {
                break;
            }
            io.run_one_for(left);
        }
        if (!done()) {
            boost::system::error_code ignored;
            socket.cancel(ignored);
            io.restart();
            while (!done() && io.run_one() > 0) {
            }
        }
    }
};

UdpSocket::UdpSocket(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

UdpSocket::~UdpSocket() {
    boost::system::error_code ignored;
    impl_->socket.close(ignored);
}

std::expected<std::unique_ptr<UdpSocket>, std::string> UdpSocket::bind(std::string_view address,
                                                                       std::uint16_t port) {
    auto impl = std::make_unique<Impl>();
    boost::system::error_code error;
    const auto endpoint = v4_endpoint(impl->io, address, port, error);
    if (!endpoint) {
        return std::unexpected(error_text(error));
    }
    impl->socket.open(udp::v4(), error);
    if (error) {
        return std::unexpected(error_text(error));
    }
    detail::disable_udp_connreset(static_cast<std::uintptr_t>(impl->socket.native_handle()));
    impl->socket.bind(*endpoint, error);
    if (error) {
        return std::unexpected(error_text(error));
    }
    return std::unique_ptr<UdpSocket>(new UdpSocket(std::move(impl)));
}

std::unique_ptr<UdpSocket> UdpSocket::unbound_broadcast() {
    auto impl = std::make_unique<Impl>();
    boost::system::error_code error;
    impl->socket.open(udp::v4(), error);
    if (!error) {
        impl->socket.set_option(asio::socket_base::broadcast(true), error);
    }
    if (error) {
        throw std::system_error(error.value(), std::system_category(), error_text(error));
    }
    detail::disable_udp_connreset(static_cast<std::uintptr_t>(impl->socket.native_handle()));
    return std::unique_ptr<UdpSocket>(new UdpSocket(std::move(impl)));
}

std::uint16_t UdpSocket::local_port() const {
    boost::system::error_code error;
    const auto local = impl_->socket.local_endpoint(error);
    return error ? 0 : local.port();
}

bool UdpSocket::send_to(std::span<const std::byte> bytes, std::string_view address,
                        std::uint16_t port) {
    Impl& d = *impl_;
    if (d.closed) {
        return false;
    }
    boost::system::error_code error;
    const auto endpoint = v4_endpoint(d.io, address, port, error);
    if (!endpoint) {
        return false;
    }
    d.socket.send_to(asio::buffer(bytes.data(), bytes.size()), *endpoint, 0, error);
    return !error;
}

std::optional<UdpSocket::Datagram> UdpSocket::receive(std::chrono::milliseconds timeout) {
    Impl& d = *impl_;
    if (d.closed) {
        d.io.restart();
        d.io.poll(); // a close() posted from another thread
        return std::nullopt;
    }
    bool finished = false;
    boost::system::error_code failure;
    std::size_t size = 0;
    udp::endpoint sender;
    d.socket.async_receive_from(asio::buffer(d.buffer), sender,
                                [&](const boost::system::error_code& error, std::size_t n) {
                                    failure = error;
                                    size = n;
                                    finished = true;
                                });
    d.run_until([&] { return finished; }, timeout);
    if (!finished || failure) {
        return std::nullopt;
    }
    Datagram datagram;
    datagram.bytes.assign(d.buffer.begin(), d.buffer.begin() + static_cast<std::ptrdiff_t>(size));
    datagram.sender = sender.address().to_string();
    datagram.port = sender.port();
    return datagram;
}

void UdpSocket::close() {
    Impl& d = *impl_;
    if (d.closed.exchange(true)) {
        return;
    }
    // Run by whichever thread is in receive(), which then returns at once; with none, by the
    // next call or the destructor.
    asio::post(d.io, [&d] {
        boost::system::error_code ignored;
        d.socket.close(ignored);
    });
}

} // namespace fv::net
