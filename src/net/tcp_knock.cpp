#include "net/tcp_knock.h"

#include <string>

// First: it sets the Windows version Asio is built for.
#include "net/detail/asio.h"

namespace fv::net {

namespace {

namespace asio = boost::asio;
using tcp = boost::asio::ip::tcp;

// One connect attempt bounded by `timeout`; the socket is closed either way.
bool knock_once(asio::io_context& io, const tcp::endpoint& endpoint,
                std::chrono::milliseconds timeout) {
    tcp::socket socket(io);
    asio::steady_timer timer(io, timeout);
    bool connected = false;
    bool finished = false;
    socket.async_connect(endpoint, [&](const boost::system::error_code& error) {
        connected = !error;
        finished = true;
        timer.cancel();
    });
    timer.async_wait([&](const boost::system::error_code& error) {
        if (!error) {
            boost::system::error_code ignored;
            socket.close(ignored); // the connect completes, aborted
        }
    });
    io.restart();
    io.run(); // both handlers complete: the connect, and the timer (fired or cancelled)
    boost::system::error_code ignored;
    socket.close(ignored);
    return finished && connected;
}

} // namespace

bool tcp_knock(std::string_view address, std::uint16_t port, std::chrono::milliseconds timeout) {
    asio::io_context io;
    tcp::resolver resolver(io);
    boost::system::error_code error;
    // socket.create_connection: getaddrinfo(host, port, 0, SOCK_STREAM), then each in turn.
    const auto results = resolver.resolve(std::string(address), std::to_string(port), error);
    if (error) {
        return false;
    }
    for (const auto& entry : results) {
        if (knock_once(io, entry.endpoint(), timeout)) {
            return true;
        }
    }
    return false;
}

} // namespace fv::net
