#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/websocket.hpp>

#include "net/detail/asio.h"
#include "net/detail/server_impl.h"
#include "net/detail/tls_context.h"
#include "net/detail/ws_keepalive.h"
#include "net/http_server.h"

// Teardown of a WebSocket over beast::ssl_stream.
#include <boost/beast/websocket/ssl.hpp>

namespace fv::net::detail {

namespace websocket = boost::beast::websocket;

// One accepted WebSocket, as uvicorn's websockets protocol serves it to Starlette (or the
// websockets server serves OpenWhisper's remote host): the upgrade answered with 101, then
// on_open, the messages, and on_close once. Every member runs on the connection's strand; the
// public entry points post there.
// Affinity: thread-safe (entry points), owned strand (the rest).
template <class Stream>
class WsSession final : public WsConnection,
                        public ServerConnection,
                        public std::enable_shared_from_this<WsSession<Stream>> {
public:
    // `open_by`: the 101 must be written by then (WsHandlers::open_timeout), else the connection
    // is dropped without an answer.
    WsSession(std::uint64_t id, std::shared_ptr<ServerImpl> server, Stream stream,
              http::request<http::string_body> upgrade, std::shared_ptr<const HttpRequest> request,
              WsHandlers handlers, Peer local, Peer remote,
              std::optional<std::chrono::steady_clock::time_point> open_by);
    ~WsSession() override;
    WsSession(const WsSession&) = delete;
    WsSession& operator=(const WsSession&) = delete;

    // Answers the upgrade; on_open follows once the 101 is out.
    void start();
    // HttpServer::stop: close 1012 (uvicorn's shutdown) after the frame being written; frames
    // not yet started are dropped.
    void server_stopping() override;
    // After the grace: close the socket now.
    void force_close() override;

    void send_text(std::shared_ptr<const std::string> text) override;
    void send_text(std::shared_ptr<const std::string> text, std::chrono::milliseconds timeout,
                   std::function<void(bool timed_out)> done) override;
    void send_binary(std::vector<std::byte> data) override;
    void close(std::uint16_t code, std::string reason) override;
    std::size_t queued_bytes() const override { return queued_bytes_.load(); }
    const HttpRequest& request() const override { return *request_; }
    std::uint64_t id() const override { return id_; }
    Peer local_endpoint() const override { return local_; }
    Peer remote_endpoint() const override { return remote_; }
    void release_message() override;
    bool is_open() const override { return open_.load(); }

private:
    struct Outgoing {
        std::uint64_t number = 0;
        std::shared_ptr<const std::string> text;
        std::vector<std::byte> binary;
        bool is_text = true;
        std::size_t size = 0;
        std::function<void(bool timed_out)> done;
        std::shared_ptr<asio::steady_timer> timer;
    };

    enum class State { Accepting, Open, Closing, Closed };

    // False, and `work` dropped, when the reactor has stopped.
    bool post(std::function<void()> work);
    void enqueue(Outgoing item, std::chrono::milliseconds timeout);
    void on_accept(const boost::system::error_code& error);
    void read_next();
    void on_read(const boost::system::error_code& error);
    void write_next();
    void on_written(const boost::system::error_code& error);
    void expire(std::uint64_t number);
    // Stops sending what is queued; each frame completes with false.
    void drop_queued();
    void begin_close(std::uint16_t code, std::string reason, std::chrono::milliseconds wait);
    void send_close();
    void schedule_ping(std::chrono::steady_clock::duration delay);
    void send_ping();
    void on_control(websocket::frame_type kind, boost::beast::string_view payload);
    // Ends the connection: the socket closes, queued frames complete, on_close runs once.
    void finish(std::uint16_t code, std::string reason);
    // A handler threw: logged as uvicorn logs it, and the connection is dropped.
    void handler_failed(const std::exception* error);
    // The close handshake's wait, set after the open deadline used the same Beast option.
    void set_close_timeout();

    std::uint64_t id_;
    std::shared_ptr<ServerImpl> server_;
    websocket::stream<Stream> ws_;
    http::request<http::string_body> upgrade_;
    std::shared_ptr<const HttpRequest> request_;
    WsHandlers handlers_;
    WsKeepalive keepalive_;
    const Peer local_;
    const Peer remote_;
    std::optional<std::chrono::steady_clock::time_point> open_by_;

    State state_ = State::Accepting;
    bool opened_ = false;
    std::atomic<bool> open_{false};
    boost::beast::flat_buffer read_buffer_;
    // Messages delivered and not yet released (WsHandlers::max_queued_messages), and whether
    // reading waits for a release.
    std::size_t unreleased_ = 0;
    bool read_paused_ = false;

    std::deque<Outgoing> out_;
    bool writing_ = false;
    std::uint64_t next_number_ = 1;
    std::atomic<std::size_t> queued_bytes_{0};

    std::uint16_t close_code_ = 1000;
    std::string close_reason_;
    bool close_sent_ = false;
    asio::steady_timer close_timer_;

    // A ping or the close frame in flight: Beast allows one control operation at a time.
    bool control_busy_ = false;
    bool close_waiting_ = false;
    asio::steady_timer ping_timer_;
    asio::steady_timer pong_timer_;
    std::optional<std::string> pending_ping_;
    std::chrono::steady_clock::time_point ping_sent_at_;
    std::chrono::steady_clock::duration last_rtt_{};
};

extern template class WsSession<PlainStream>;
extern template class WsSession<TlsStream>;

} // namespace fv::net::detail
