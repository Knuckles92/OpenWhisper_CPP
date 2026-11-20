// The discovery sockets (discovery.py's responder and search) and the sweep's knock, on
// 127.0.0.1 only. Broadcasting is never done here: a broadcast leaves this computer.

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

#include <chrono>
#include <cstddef>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "net/tcp_knock.h"
#include "net/udp.h"

using namespace fv::net;
using namespace std::chrono_literals;

namespace {

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out;
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

std::string text_of(const std::vector<std::byte>& bytes) {
    std::string out;
    for (const auto b : bytes) {
        out += static_cast<char>(b);
    }
    return out;
}

// A UDP port nothing listens on: bound, then released.
std::uint16_t closed_udp_port() {
    auto probe = UdpSocket::bind("127.0.0.1", 0);
    REQUIRE(probe);
    return (*probe)->local_port();
}

} // namespace

TEST_CASE("a datagram goes from the searching socket to the responder and back", "[platform]") {
    auto responder = UdpSocket::bind("127.0.0.1", 0);
    REQUIRE(responder);
    const std::uint16_t port = (*responder)->local_port();
    CHECK(port != 0);

    auto searcher = UdpSocket::unbound_broadcast();
    REQUIRE(searcher);
    CHECK(searcher->local_port() == 0); // no bind until the first send
    REQUIRE(searcher->send_to(bytes_of("OWHISPER?1 query"), "127.0.0.1", port));
    CHECK(searcher->local_port() != 0);

    const auto query = (*responder)->receive(5s);
    REQUIRE(query);
    CHECK(text_of(query->bytes) == "OWHISPER?1 query");
    CHECK(query->sender == "127.0.0.1");
    CHECK(query->port == searcher->local_port());

    REQUIRE((*responder)->send_to(bytes_of("reply"), query->sender, query->port));
    const auto reply = searcher->receive(5s);
    REQUIRE(reply);
    CHECK(text_of(reply->bytes) == "reply");
    CHECK(reply->port == port);
}

TEST_CASE("a second bind of a taken port returns the OS message", "[platform]") {
    auto first = UdpSocket::bind("127.0.0.1", 0);
    REQUIRE(first);
    const auto second = UdpSocket::bind("127.0.0.1", (*first)->local_port());
    REQUIRE_FALSE(second);
    // OSError.strerror: the system's sentence without its period or line end.
    INFO(second.error());
    CHECK_FALSE(second.error().empty());
    CHECK_FALSE(second.error().ends_with("."));
    CHECK_FALSE(second.error().ends_with("\n"));
    CHECK_FALSE(second.error().ends_with("\r"));
#ifndef _WIN32
    CHECK(second.error() == "Address already in use");
#endif
    // On Windows it is WSAEADDRINUSE's sentence in the system's language ("Only one usage of
    // each socket address (protocol/network address/port) is normally permitted" in English).
}

TEST_CASE("receive times out with nothing to read", "[platform]") {
    auto socket = UdpSocket::bind("127.0.0.1", 0);
    REQUIRE(socket);
    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE((*socket)->receive(150ms));
    const auto waited = std::chrono::steady_clock::now() - started;
    CHECK(waited >= 140ms);
    CHECK(waited < 2s);
}

TEST_CASE("close unblocks a receiver on another thread", "[platform]") {
    auto socket = UdpSocket::bind("127.0.0.1", 0);
    REQUIRE(socket);
    UdpSocket& shared = **socket;
    auto receiver = std::async(std::launch::async, [&] { return shared.receive(10s); });
    std::this_thread::sleep_for(100ms);
    const auto started = std::chrono::steady_clock::now();
    shared.close();
    REQUIRE(receiver.wait_for(5s) == std::future_status::ready);
    CHECK_FALSE(receiver.get());
    CHECK(std::chrono::steady_clock::now() - started < 2s);
    CHECK_FALSE(shared.receive(10ms));
    CHECK_FALSE(shared.send_to(bytes_of("x"), "127.0.0.1", 9));
}

TEST_CASE("a send to a closed port does not break the next receive", "[platform]") {
    // Windows reports the ICMP "port unreachable" on the next recvfrom unless
    // SIO_UDP_CONNRESET is off (discovery.py ignored it; here it is off).
    auto socket = UdpSocket::bind("127.0.0.1", 0);
    REQUIRE(socket);
    const std::uint16_t closed = closed_udp_port();
    REQUIRE((*socket)->send_to(bytes_of("nobody"), "127.0.0.1", closed));
    CHECK_FALSE((*socket)->receive(200ms));

    auto sender = UdpSocket::unbound_broadcast();
    REQUIRE(sender->send_to(bytes_of("after"), "127.0.0.1", (*socket)->local_port()));
    const auto got = (*socket)->receive(5s);
    REQUIRE(got);
    CHECK(text_of(got->bytes) == "after");
}

TEST_CASE("tcp_knock answers for a listening port", "[platform]") {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor listening(io, {boost::asio::ip::make_address("127.0.0.1"), 0});
    CHECK(tcp_knock("127.0.0.1", listening.local_endpoint().port(), 400ms));
}

TEST_CASE("tcp_knock is false for a closed port within the timeout", "[platform]") {
    std::uint16_t port = 0;
    {
        boost::asio::io_context io;
        boost::asio::ip::tcp::acceptor taken(io, {boost::asio::ip::make_address("127.0.0.1"), 0});
        port = taken.local_endpoint().port();
    }
    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(tcp_knock("127.0.0.1", port, 400ms));
    // Windows retries a refused SYN for about 2 s; the knock gives up at its timeout.
    CHECK(std::chrono::steady_clock::now() - started < 1500ms);
}
