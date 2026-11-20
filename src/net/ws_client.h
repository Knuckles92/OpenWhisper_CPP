#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "net/reactor.h"
#include "net/types.h"

namespace fv::net {

// Why a WebSocket connection did not open (the remote client words each differently).
struct WsConnectFailure {
    enum class Kind {
        Dns,         // the host name did not resolve
        Refused,     // nothing accepts connections on that port (ECONNREFUSED)
        Unreachable, // no route, reset, or any other socket error
        Timeout,     // connect_timeout passed first
        Tls,         // the TLS handshake failed, or the pinned certificate did not match
        HttpStatus,  // the server answered the upgrade with another status (http_status)
        Handshake,   // the server's answer was no WebSocket handshake, or the URL was unusable
        Cancelled,   // closed by this side, or the reactor stopped, before it opened
    };
    Kind kind = Kind::Unreachable;
    int http_status = 0;
    std::string detail;
};

// A WebSocket client connection (remote engine), as websockets' connect(compression=None,
// ping_interval=None) opens one: no permessage-deflate, no keepalive of its own, each message
// one frame. The callbacks run on the reactor, one at a time.
struct WsClientOptions {
    std::string url; // ws:// or wss://
    Headers headers;
    // Uppercase hex SHA-256 of the server certificate's DER (a17's certificate_fingerprint). Set,
    // the connection is refused unless the certificate matches, whatever its chain says.
    std::optional<std::string> pinned_sha256;
    // TCP connect, TLS and the upgrade together (websockets' open_timeout).
    std::chrono::milliseconds connect_timeout{10'000};
    std::function<void()> on_open;
    std::function<void(std::string_view text)> on_text;
    std::function<void(std::span<const std::byte>)> on_binary;
    // Also the failure path: a refused or lost connection closes with a code and a reason.
    std::function<void(std::uint16_t code, std::string_view reason)> on_close;
    // Set, a connection that never opened reports here (once, instead of on_close); unset, it
    // closes 1006 with the failure's detail as the reason.
    std::function<void(const WsConnectFailure&)> on_connect_failed;
    // A larger message fails the connection with 1009 (the remote client's MAX_REPLY_BYTES).
    std::size_t max_message_bytes = 8u << 20;
    // After close(), how long the peer's close frame is waited for before the TCP connection
    // is dropped.
    std::chrono::milliseconds close_timeout{2'000};
};

// Affinity: thread-safe.
class WsClientConnection : public WsConnection {
public:
    // Open from on_open until the connection closed or close() was called.
    bool is_open() const override = 0;
    // The SHA-256 of the certificate the server presented (uppercase hex), once connected.
    virtual std::optional<std::string> peer_certificate_sha256() const = 0;
    // Sends a ping; done gets the round trip (steady clock) or nullopt when no pong came within
    // timeout or the socket closed. A pong answers its own ping and every earlier one, as
    // websockets acknowledges them. Runs on the reactor.
    virtual void ping(std::chrono::milliseconds timeout,
                      std::function<void(std::optional<std::chrono::nanoseconds>)> done) = 0;
    // websockets' close_code, once closed: the code of the peer's close frame (1005 when it had
    // none), 1006 when none arrived or this side failed the connection (a message over
    // max_message_bytes sends 1009, and the peer's answer is ignored); nullopt while the
    // connection is open or opening.
    virtual std::optional<std::uint16_t> close_code() const = 0;
};

// Connects in the background; every outcome arrives through the options' callbacks. wss://
// verifies neither the certificate's chain nor its name (the host is self-signed and reached by
// IP, name or tailnet address alike) and sends SNI only for a name. on_close reports
// close_code() and the peer's reason (this side's reason when the peer sent none).
// Dropping the returned connection closes it with 1000, and from then on no callback runs, a
// ping's done included: the destructor waits for a callback running on another thread, so a
// thread must not drop it while holding a lock that a callback takes.
std::unique_ptr<WsClientConnection> ws_connect(Reactor&, WsClientOptions); // SHA-256 pinning

} // namespace fv::net
