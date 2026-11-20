#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/cancel.h"
#include "base/json.h"
#include "net/reactor.h"
#include "net/types.h"
#include "net/ws_client.h"

// What websockets' synchronous client gives client.py, over net::ws_connect: a blocking open,
// sends, and receives with a timeout. Messages that arrived before the host closed are still
// received, so a refusal the host sent just before closing reaches the caller.
namespace fv::remote::detail {

// One frame received: a text frame's UTF-8, or a binary frame's bytes.
struct Message {
    bool binary = false;
    std::string data;
};

enum class ReceiveError { timeout, closed, canceled };

// Affinity: thread-safe. The destructor drops the connection at once (close() first for the
// closing handshake); it must not run while a callback of this socket waits on a lock the caller
// holds.
class ClientSocket {
public:
    // Connects to `url` within `timeout` (TCP, TLS and the upgrade together), with no
    // permessage-deflate, no keepalive of its own and replies of at most MAX_REPLY_BYTES.
    static std::expected<std::shared_ptr<ClientSocket>, net::WsConnectFailure>
    open(net::Reactor& reactor, const std::string& url, std::chrono::milliseconds timeout);

    ~ClientSocket();
    ClientSocket(const ClientSocket&) = delete;
    ClientSocket& operator=(const ClientSocket&) = delete;

    // The uppercase hex SHA-256 of the certificate the host presented.
    std::string fingerprint() const;
    // The other end's numeric address; empty when the socket can't say.
    std::string peer_address() const;
    // False when the connection has closed (websockets raises ConnectionClosed).
    bool send_text(std::string text);
    bool send_binary(std::vector<std::byte> bytes);
    // The next message, else timeout or closed (once no message is left).
    std::expected<Message, ReceiveError> receive(std::chrono::milliseconds timeout,
                                                 const CancelToken& cancel = {});
    // websockets' State.OPEN: neither side has begun closing.
    bool is_open() const;
    // The close code once closed (1006 when lost); nullopt before.
    std::optional<std::uint16_t> close_code() const;
    // An observer of this connection's close frame or transport shutdown. May call immediately
    // if the reactor already closed it. The callback must never wait on network work.
    void watch_closed(std::function<void(std::uint16_t)>);
    // A ping; `done` gets the round trip, or nullopt when no pong came within `timeout` or the
    // connection closed. Runs on the reactor.
    void ping(std::chrono::milliseconds timeout,
              std::function<void(std::optional<std::chrono::nanoseconds>)> done);
    // Closes 1000 and waits for the closing handshake, at most CLOSING_TIMEOUT and a second more:
    // a frame the host stopped reading holds the close frame back, and this never waits past
    // that.
    void close();

    struct State;

private:
    ClientSocket() = default;
    std::shared_ptr<State> state_;
    std::unique_ptr<net::WsClientConnection> connection_;
};

// _open: the socket, or the RemoteEngineError (RemoteUnreachable for no answer, a refusal or an
// unknown name) that says why not in plain language.
std::shared_ptr<ClientSocket> open_engine_socket(net::Reactor& reactor, std::string_view host,
                                                 int port, std::chrono::milliseconds timeout);
// The exception _open raises for a connection that failed this way.
[[noreturn]] void throw_open_failure(const net::WsConnectFailure& failure, std::string_view host,
                                     int port);

// _quiet_close: close() without letting a slow closing handshake hide the real error.
void quiet_close(ClientSocket& socket) noexcept;

// _read_json: the next message as a JSON object. RemoteEngineError "The host didn't answer in
// time.", "The host closed the connection." or "The host sent a reply this version can't read.".
Json read_json(ClientSocket& socket, std::chrono::milliseconds timeout,
               const CancelToken& cancel = {});

// json.loads of a text frame's str or a binary frame's bytes; nullopt where Python raises
// ValueError or TypeError.
std::optional<Json> loads_message(const Message& message);

// hmac.compare_digest over two str: constant time for equal lengths; py::TypeError when either
// holds a non-ASCII character, as Python refuses them.
bool compare_digest(std::string_view a, std::string_view b);

// bytes.fromhex(text): pairs of hex digits, ASCII whitespace allowed between them; nullopt
// where Python raises ValueError.
std::optional<std::vector<std::uint8_t>> from_hex(std::string_view text);

// "a FioriVoce" or "an <product>", by the product name's first letter: the article OpenWhisper's
// copy writes before its own name follows the name that replaces it.
std::string a_product();

// type(exc).__name__ for a log line that must not carry the exception's text.
std::string exception_class(const std::exception& error);

} // namespace fv::remote::detail
