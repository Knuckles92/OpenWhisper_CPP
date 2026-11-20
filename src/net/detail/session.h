#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <boost/beast/core/tcp_stream.hpp>

#include "net/detail/asio.h"
#include "net/detail/file_response.h"
#include "net/detail/server_impl.h"
#include "net/detail/tls_context.h"
#include "net/http_server.h"

namespace fv::net::detail {

class SseStream;

// What a Responder and an SseStream need of the connection that answers them.
// Affinity: thread-safe.
class HttpConnection : public ServerConnection {
public:
    // `request` is the request's number on this connection; an answer for one that is gone is
    // dropped.
    virtual void respond(std::uint64_t request, int status, Headers headers, std::string body) = 0;
    // The head, then `file` as the socket takes it (or `body` when there is no file).
    virtual void respond_file(std::uint64_t request, int status, Headers headers, std::string body,
                              std::shared_ptr<FileBody> file) = 0;
    virtual void start_sse(std::uint64_t request, Headers headers,
                           std::shared_ptr<SseStream> stream) = 0;
    // A frame or the end of the stream answering `request`; for another request they are dropped.
    virtual void sse_write(std::uint64_t request, std::string frame) = 0;
    virtual void sse_end(std::uint64_t request) = 0;
    // SSE frames handed to sse_write and not yet taken by the socket.
    virtual std::size_t queued_frames() const = 0;
    virtual bool reactor_stopped() const = 0;
};

// One HTTP/1.1 connection, plain (PlainStream) or TLS (TlsStream): requests one at a time
// (keep-alive), each answered through a Responder, or held open as an SSE stream. Every member
// runs on the connection's strand; the public entry points post there.
// Affinity: thread-safe (entry points), owned strand (the rest).
template <class Stream>
class Session final : public HttpConnection, public std::enable_shared_from_this<Session<Stream>> {
public:
    // `accepted` is when the TCP connection was accepted: the open deadline counts from it.
    Session(std::uint64_t id, std::shared_ptr<ServerImpl> server, Stream stream,
            std::chrono::steady_clock::time_point accepted);
    ~Session() override;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void start();
    // HttpServer::stop: close at once when idle, else after the answer in progress.
    void server_stopping() override;
    // After the grace: close now, SSE streams included.
    void force_close() override;

    void respond(std::uint64_t request, int status, Headers headers, std::string body) override;
    void respond_file(std::uint64_t request, int status, Headers headers, std::string body,
                      std::shared_ptr<FileBody> file) override;
    void start_sse(std::uint64_t request, Headers headers,
                   std::shared_ptr<SseStream> stream) override;
    void sse_write(std::uint64_t request, std::string frame) override;
    void sse_end(std::uint64_t request) override;
    std::size_t queued_frames() const override { return queued_frames_.load(); }
    bool reactor_stopped() const override;

private:
    // Bytes waiting for the socket; `frame` marks an SSE frame counted in queued_frames_.
    struct Pending {
        std::string bytes;
        bool frame = false;
    };

    enum class State { Reading, Handling, Streaming, Closing, Closed };

    void post(std::function<void()> work);
    void handshake();
    void read_header();
    void on_header(const boost::system::error_code& error);
    void read_body(std::size_t max_body);
    void handle();
    // Status line, date, the handler's headers with their names in lower case (as Starlette
    // sends them), then content-length or chunked framing and "connection: close" as needed.
    std::string response_head(int status, const Headers& headers, bool streaming,
                              std::size_t body_size, bool& closes) const;
    void write_response(int status, Headers headers, std::string body);
    void send_400();
    void enqueue(std::string bytes, bool frame = false);
    void write_next();
    void after_flush();
    // The next chunk of the file body, once the socket took the last one.
    void pump_file();
    // A WebSocket upgrade: the socket moves to a WsSession, or the WebSocket fallback answers,
    // or 403 when nothing takes it.
    void upgrade();
    void watch_peer();
    void close_gracefully();
    void drain();
    void close_now();
    // The open SSE stream, if its owner still holds it, is told the client has gone.
    void release_stream();
    // The lowest layer: the TCP stream with its timeouts and socket.
    beast::tcp_stream& lowest() { return beast::get_lowest_layer(stream_); }

    std::uint64_t id_;
    std::shared_ptr<ServerImpl> server_;
    Stream stream_;
    asio::steady_timer drain_timer_;
    boost::beast::flat_buffer buffer_;
    std::optional<http::request_parser<http::string_body>> parser_;
    Peer peer_;
    Endpoint local_;
    // When the request now being read began: accept for the first, else the end of the last.
    std::chrono::steady_clock::time_point request_began_;

    State state_ = State::Reading;
    bool reading_body_ = false;
    std::uint64_t request_number_ = 0;
    std::shared_ptr<HttpRequest> request_;
    std::string raw_query_;
    Dispatch dispatch_;
    bool keep_alive_ = true;
    bool close_after_ = false;
    bool head_request_ = false;
    bool expect_continue_ = false;
    bool first_request_ = true;
    bool stopping_ = false;

    std::deque<Pending> out_;
    bool writing_ = false;
    // What to do once out_ is empty: read the next request, or close.
    bool finish_after_flush_ = false;
    std::atomic<std::size_t> queued_frames_{0};

    // A file response being sent.
    std::shared_ptr<FileBody> file_body_;

    // Weak: the stream's owner decides how long it lives, and dropping it ends the stream.
    std::weak_ptr<SseStream> sse_;
    std::array<char, 512> scratch_{};
};

extern template class Session<PlainStream>;
extern template class Session<TlsStream>;

// The EventStream start_sse returns. It answers request number `request` of its session.
// Affinity: thread-safe.
class SseStream final : public EventStream {
public:
    SseStream(std::shared_ptr<HttpConnection> session, std::uint64_t request)
        : session_(std::move(session)), request_(request) {}
    ~SseStream() override;
    SseStream(const SseStream&) = delete;
    SseStream& operator=(const SseStream&) = delete;

    bool write(std::string frame) override;
    void close() override;
    std::size_t queued_frames() const override;
    void mark_gone() { gone_ = true; }
    bool gone() const { return gone_.load(); }

private:
    std::shared_ptr<HttpConnection> session_;
    std::uint64_t request_;
    std::atomic<bool> gone_{false};
    std::atomic<bool> closed_{false};
};

} // namespace fv::net::detail
