#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "net/reactor.h"
#include "net/types.h"

namespace fv::net {

// Valid only while the handler runs; a handler that answers later copies what it needs.
struct HttpRequest {
    std::string method, path; // path percent-decoded, as Starlette's scope["path"]
    Query query;
    Headers headers;
    std::string body;
    Peer peer;
    bool tls;
    std::vector<std::pair<std::string, std::string>> params; // "{id}" segments of the pattern
    // Content-Length or a streamed body over RouteOptions::max_body. The body is then empty
    // (never read), and the connection closes after the response.
    bool body_too_large = false;
};

// What Starlette's FileResponse and StaticFiles take beside the path and the media type.
struct FileOptions {
    // Sent before the file's own headers, names in lower case (FileResponse's headers=).
    Headers headers;
    // Content-Disposition `<type>; filename="<name>"`, or `filename*=utf-8''<quoted>` for a name
    // quote() changes (FileResponse's filename= and content_disposition_type=).
    std::optional<std::string> filename;
    std::string content_disposition_type = "attachment";
    // StaticFiles: 304 Not Modified, with only the cache headers, when If-None-Match names the
    // ETag or If-Modified-Since is not older than the file.
    bool not_modified_check = false;
};

// shared_ptr; may complete later from any thread. Exactly one of send, send_file and start_sse
// answers a request; a second call is ignored and logged. Dropped without an answer, it sends
// 500.
// Affinity: thread-safe.
class Responder {
public:
    virtual ~Responder() = default;
    virtual void send(int status, Headers, std::string body) = 0;
    // Starlette's FileResponse(path, media_type=content_type): accept-ranges, content-length,
    // last-modified and an ETag; text/* gets "; charset=utf-8"; an empty content_type is guessed
    // from the path (content_type_for). Without `range`, the request's own Range and If-Range
    // headers decide, as FileResponse reads them: one range 206 with Content-Range, several a
    // multipart/byteranges 206, a range past the end 416 with "Content-Range: bytes */<size>",
    // a malformed one 400 with Starlette's message. A given `range` is used instead of the
    // header (`first` unset: the last `last` bytes; `last` unset: to the end). The body is streamed
    // in 64 KiB chunks, read as the socket takes them; HEAD gets the head alone. A path that is not
    // a file answers FastAPI's 404 {"detail":"Not Found"}.
    virtual void send_file(std::filesystem::path, std::string content_type,
                           std::optional<ByteRange>) = 0; // 206 and 416 handled
    // The same with FileResponse's other arguments, or StaticFiles' conditional answer. The
    // default ignores `options`.
    virtual void send_file(std::filesystem::path path, std::string content_type,
                           std::optional<ByteRange> range, FileOptions options) {
        (void)options;
        send_file(std::move(path), std::move(content_type), range);
    }
    // Status 200 with the headers given, sent at once; frames go out chunked as written.
    virtual std::shared_ptr<EventStream> start_sse(Headers) = 0;
};

// With `tls`, every connection on this listener is TLS (1.2 at least) with that certificate,
// and HttpRequest::tls is true. Its handshake must end within asyncio's 60 s, or within the
// shortest WsHandlers::open_timeout of the server's WebSocket routes, which then also bounds the
// arrival of the first request (websockets' open_timeout covers TLS, request and upgrade). A
// certificate or key OpenSSL cannot use fails listen().
struct Listen {
    std::string address;
    std::uint16_t port;
    std::optional<TlsIdentity> tls;
    bool exclusive = true; // SO_EXCLUSIVEADDRUSE on Windows, SO_REUSEADDR elsewhere
};

struct RouteOptions {
    std::size_t max_body = 1 << 20; // bytes
    // Where the handler runs; null runs it on the reactor, where it must not block. Python's
    // sync FastAPI handlers ran in a thread pool: pass the library's io strand.
    Executor* run_on = nullptr;
};

// Callbacks of one WebSocket route. They run on the reactor and never block. Every upgrade
// request to the path is accepted (the 101 goes out first), then on_open runs, then the
// messages in arrival order, then on_close once: with the code this side closed with (1012
// when the server stops, 1009 for a message over the cap, 1007 for text that is not UTF-8,
// 1011 when keepalive pings go unanswered), else the peer's code and reason, else 1005 when the
// connection was lost. Messages that arrive after close() are dropped, as uvicorn drops them.
// Keepalive: a ping every 20 s, closed 1011 "keepalive ping timeout" when its pong takes 20 s
// (uvicorn's ws_ping_interval and ws_ping_timeout), unless the route sets its own.
// permessage-deflate is offered as uvicorn offers it (12-bit windows), unless the route turns it
// off.
struct WsHandlers {
    // After the upgrade; the request carries the path, query and headers (tokens, Origin).
    std::function<void(std::shared_ptr<WsConnection>, const HttpRequest&)> on_open;
    std::function<void(const std::shared_ptr<WsConnection>&, std::string_view text)> on_text;
    std::function<void(const std::shared_ptr<WsConnection>&, std::span<const std::byte>)> on_binary;
    std::function<void(const std::shared_ptr<WsConnection>&, std::uint16_t code,
                       std::string_view reason)>
        on_close;
    std::size_t max_message_bytes = 1 << 20; // a larger frame closes 1009; the hub checks 65,536
                                             // chars itself
    // From TCP accept to the 101 being written, TLS included; a connection that takes longer is
    // dropped without an answer (websockets' open_timeout). 0: no limit.
    std::chrono::milliseconds open_timeout{10'000};
    // Keepalive of this route; 0 keeps the server's default (uvicorn's 20 s each). The remote
    // host sets 20 s, as websockets' serve(ping_interval=20, ping_timeout=20).
    std::chrono::milliseconds ping_interval{0};
    std::chrono::milliseconds ping_timeout{0}; // pong deadline; missed -> close 1011
    // 0: read freely. n: stop reading the socket while n delivered messages are not released
    // (WsConnection::release_message), as websockets' max_queue pauses its reader; frames wait
    // in the TCP buffers, none is dropped.
    std::size_t max_queued_messages = 0;
    // Offer permessage-deflate when the client asks for it (uvicorn does; the remote host
    // serves with compression=None).
    bool permessage_deflate = true;
};

namespace detail {
class ServerImpl;
} // namespace detail

// Every server (meeting dashboard, MCP, live API and SSE, History REST, remote-engine host,
// loopback MCP for agents) is an HttpServer on the one Reactor. Handlers are pure functions in
// their own libraries, tested without sockets.
//
// HTTP/1.1 with keep-alive (5 s idle), as uvicorn serves Starlette: a pattern's "{name}" matches
// one path segment and "{name:path}" the rest of the path; routes are tried in the order added;
// HEAD runs a GET route without sending the body; a path that matches no route but does with
// its trailing slash added or removed is redirected (307), as Starlette's redirect_slashes.
// Without a fallback, 404 and 405 answer FastAPI's {"detail": "Not Found"} and
// {"detail": "Method Not Allowed"}. A handler that throws is logged and answered with 500.
// Affinity: thread-safe; stop() and the destructor wait, so they are not called from the
// reactor's threads.
class HttpServer {
public:
    using Handler = std::function<void(const HttpRequest&, std::shared_ptr<Responder>)>;
    using Fallback =
        std::function<void(const HttpRequest&, bool path_known, std::shared_ptr<Responder>)>;

    static std::unique_ptr<HttpServer> create(Reactor&);
    ~HttpServer(); // stop(0) when stop() was not called
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void route(std::string method, std::string pattern,
               std::function<void(const HttpRequest&, std::shared_ptr<Responder>)>);
    void route(std::string method, std::string pattern, RouteOptions,
               std::function<void(const HttpRequest&, std::shared_ptr<Responder>)>);
    // Called for a path no route matches (path_known false) or a known path with another method.
    // It runs on the reactor.
    void set_fallback(
        std::function<void(const HttpRequest&, bool path_known, std::shared_ptr<Responder>)>);
    // Upgrade on the same port, for exactly this path (query aside). A WebSocket request to a
    // path without one is refused with 403, as Starlette refuses it, unless a WebSocket fallback
    // is set.
    void websocket(std::string path, WsHandlers); // upgrade on the same port
    // Answers a WebSocket upgrade request to a path no websocket() route takes, with no upgrade
    // (websockets' process_request: the remote host's 404). The connection closes after the
    // answer. It runs on the reactor.
    void
        set_websocket_fallback(std::function<void(const HttpRequest&, std::shared_ptr<Responder>)>);
    // May be called several times (loopback and Tailscale). A failure closes every listener
    // this server opened, so a start that binds several addresses leaves none behind.
    std::expected<Endpoint, std::string> listen(const Listen&);
    // Closes the listeners and idle keep-alive connections at once, lets requests in progress
    // and SSE streams run for at most `grace`, then closes what is left. Never waits for a
    // handler: one still running answers into a closed connection, which drops the answer.
    void stop(std::chrono::milliseconds grace); // aborts streaming after grace

private:
    explicit HttpServer(std::shared_ptr<detail::ServerImpl>);
    std::shared_ptr<detail::ServerImpl> impl_;
};

// The one range a Range header asks for, as written: "a-b" is {a, b} (inclusive), "a-" is
// {a, unset} (to the end), "-n" is {unset, n} (the last n bytes), the form send_file takes.
// nullopt when the header is not a bytes range of exactly one numeric part. `size` is not
// consulted: the range may lie outside the file (`first` at or past it, "-0", or `last` before
// `first`), and send_file answers that 416 or 400 as Starlette does.
std::optional<ByteRange> parse_range(std::string_view header, std::uint64_t size);

// mimetypes.guess_type(path)[0] with Python 3.12's built-in table (the Windows registry is not
// read), plus ".woff" font/woff and ".woff2" font/woff2; "application/octet-stream" when unknown.
std::string content_type_for(const std::filesystem::path&);

} // namespace fv::net
