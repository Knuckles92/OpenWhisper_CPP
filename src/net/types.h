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
#include <utility>
#include <vector>

namespace fv::net {

// Header and query pairs in arrival order; names may repeat.
using Headers = std::vector<std::pair<std::string, std::string>>;
using Query = std::vector<std::pair<std::string, std::string>>; // percent-decoded

// The value of the first header of that name, compared without regard to ASCII case, as
// Starlette's request.headers.get finds it. The view points into the Headers.
std::optional<std::string_view> header(const Headers&, std::string_view name);

// The remote end of a connection.
struct Peer {
    std::string address; // numeric IPv4 or IPv6
    std::uint16_t port = 0;
};

// Where a server actually listens (port 0 in Listen becomes the bound port).
struct Endpoint {
    std::string address;
    std::uint16_t port = 0;
};

// One HTTP byte range: bytes=first-last, bytes=first- (no last) or bytes=-N (no first: last is
// the suffix length N). Inclusive, as in the header.
struct ByteRange {
    std::optional<std::uint64_t> first;
    std::optional<std::uint64_t> last;
};

// A server's certificate and private key (PEM). disable_tickets sends no TLS 1.3 session tickets
// (OpenWhisper's remote host: a websockets client stalled on them on Windows).
struct TlsIdentity {
    std::string cert_pem, key_pem;
    bool disable_tickets = false;
};

// A Server-Sent Events response (live API, MCP events) held open after start_sse.
// Affinity: thread-safe.
class EventStream {
public:
    virtual ~EventStream() = default;
    // One complete, already formatted frame ("event: ...\ndata: ...\n\n" or a ": comment\n\n"
    // keep-alive). False once the client has gone; the frame is then dropped.
    virtual bool write(std::string frame) = 0;
    // Ends the stream after the frames already written. Dropping the last reference to the
    // stream without calling it does the same.
    virtual void close() = 0;
    // Frames written but not yet taken by the socket. write never blocks, so this is where a
    // client that reads too slowly shows: uvicorn made the generator wait instead, and the live
    // API's queue of 1,000 events filled (api.py STREAM_QUEUE_SIZE).
    virtual std::size_t queued_frames() const { return 0; }
};

struct HttpRequest;

// One WebSocket. Frames go out in the order sent; a frame sent after close() or after the
// connection ended is dropped. close() sends a close frame after the frames already queued
// (codes 1000, 1011 and the application's 4000-4999 reach the browser as `event.code`).
// Affinity: thread-safe.
class WsConnection {
public:
    virtual ~WsConnection() = default;
    virtual void send_text(std::shared_ptr<const std::string>) = 0; // one buffer, N sockets
    virtual void send_binary(std::vector<std::byte>) = 0;
    virtual void close(std::uint16_t code, std::string reason) = 0;
    virtual std::size_t queued_bytes() const = 0;

    // Completes once the frame is written or `timeout` passes (true when it timed out); runs on
    // the reactor. The hub's 5 s send timeout and 3-strike drop use it. A frame that times out
    // before its first byte went out is dropped, as a cancelled send is; one already going out
    // finishes. A frame the connection drops (closed) completes with false. The default sends
    // it untimed and completes at once.
    virtual void send_text(std::shared_ptr<const std::string> text,
                           std::chrono::milliseconds timeout,
                           std::function<void(bool timed_out)> done);
    // The upgrade request (path, query: token, name, guest_id; headers). The default is empty.
    virtual const HttpRequest& request() const;
    // Unique among the connections of one server. The default is 0.
    virtual std::uint64_t id() const;
    // This end and the other end of the TCP connection (numeric addresses); empty by default.
    virtual Peer local_endpoint() const { return {}; }
    virtual Peer remote_endpoint() const { return {}; }
    // One message delivered to on_text or on_binary has been handled: with
    // WsHandlers::max_queued_messages set, reading resumes once fewer than that many delivered
    // messages are unreleased. Thread-safe; without the limit it does nothing.
    virtual void release_message() {}
    // Until the connection has closed (or this side began closing it).
    virtual bool is_open() const { return true; }
};

} // namespace fv::net
