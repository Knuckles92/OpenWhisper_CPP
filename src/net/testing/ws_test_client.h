#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "net/types.h"

namespace fv::net::testing {

// A blocking WebSocket client for tests, on its own io_context, to 127.0.0.1 only. It is not
// ws_connect (the product's client). Work happens only inside its calls: between them nothing
// reads the socket, which is how a test plays a client that stopped reading (and so answers no
// ping).
// Affinity: one thread at a time.
class WsTestClient {
public:
    struct Message {
        bool binary = false;
        std::string data;
    };

    struct Options {
        Headers headers;
        // SO_RCVBUF before connecting, so a stalled client fills up sooner; 0 keeps the default.
        int receive_buffer_bytes = 0;
        // Offer permessage-deflate, as browsers do.
        bool deflate = false;
        std::chrono::milliseconds timeout{10'000};
        // wss://: TLS 1.2 or later, any certificate (the server's is self-signed).
        bool tls = false;
    };

    WsTestClient();
    ~WsTestClient();
    WsTestClient(const WsTestClient&) = delete;
    WsTestClient& operator=(const WsTestClient&) = delete;

    // Connects to 127.0.0.1:port and upgrades `path_and_query`. On a refused upgrade the error
    // names the HTTP status ("HTTP 403").
    bool connect(std::uint16_t port, std::string_view path_and_query);
    bool connect(std::uint16_t port, std::string_view path_and_query, const Options& options);
    const std::string& error() const;

    void send_text(std::string_view text);
    void send_binary(std::span<const std::byte> data);
    // The next message, or nullopt on timeout or once the connection has closed.
    std::optional<Message> receive(std::chrono::milliseconds timeout);
    // Waits up to `timeout` for the server to close; true when it did.
    bool wait_closed(std::chrono::milliseconds timeout);
    // Set once the server's close frame arrived (or 1006 when the connection was lost).
    std::optional<std::uint16_t> close_code() const;
    std::string close_reason() const;
    // Sends a close frame and waits for the server's answer.
    void close(std::uint16_t code = 1000, std::string_view reason = {});
    // Drops the TCP connection without a close frame.
    void abort();
    // Pings the server sent that this client read (and answered) so far.
    int pings_received() const;
    // Over TLS, the uppercase hex SHA-256 of the server's certificate; empty otherwise.
    std::string peer_certificate_sha256() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fv::net::testing
