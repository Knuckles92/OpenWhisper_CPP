#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "net/detail/asio.h"
#include "net/detail/reactor_impl.h"
#include "net/detail/router.h"
#include "net/detail/server_connection.h"
#include "net/http_server.h"

namespace boost::asio::ssl {
class context;
} // namespace boost::asio::ssl

namespace fv::net::detail {

// What a session needs to answer one request, copied out of the router under its lock.
struct Dispatch {
    HttpServer::Handler handler;   // set for a full match
    HttpServer::Fallback fallback; // set for 404 and 405 when the server has one
    RouteOptions options;
    Params params;
    bool path_known = false;
    std::vector<std::string> allowed;
    std::optional<std::string> redirect_path;
};

// One listening socket and its accept loop, on its own strand.
struct Listener {
    explicit Listener(asio::io_context& io)
        : acceptor(asio::make_strand(io)), retry(acceptor.get_executor()) {}
    tcp::acceptor acceptor;
    asio::steady_timer retry;                       // the next accept after a failed one
    std::shared_ptr<boost::asio::ssl::context> tls; // set for a TLS listener
};

// HttpServer's state, shared with its sessions.
// Affinity: thread-safe.
class ServerImpl : public std::enable_shared_from_this<ServerImpl> {
public:
    explicit ServerImpl(std::shared_ptr<ReactorImpl> reactor);

    void add_route(std::string method, std::string_view pattern, RouteOptions options,
                   HttpServer::Handler handler);
    void set_fallback(HttpServer::Fallback fallback);
    Dispatch dispatch_for(std::string_view method, std::string_view path) const;
    void add_websocket(std::string path, WsHandlers handlers);
    std::optional<WsHandlers> websocket_for(const std::string& path) const;
    void set_websocket_fallback(HttpServer::Handler fallback);
    HttpServer::Handler websocket_fallback() const;
    // The shortest positive open_timeout of the WebSocket routes, which bounds a TLS
    // connection's handshake and first request; nullopt when no route sets one.
    std::optional<std::chrono::milliseconds> open_deadline() const;

    std::expected<Endpoint, std::string> listen(const Listen& where);
    void stop(std::chrono::milliseconds grace);
    bool stopping() const { return stopping_.load(); }

    const std::shared_ptr<ReactorImpl>& reactor() const { return reactor_; }
    void session_closed(std::uint64_t id);
    // A connection upgraded to a WebSocket keeps its id; stop() closes it with the others.
    void ws_opened(std::uint64_t id, const std::shared_ptr<ServerConnection>& session);
    void ws_closed(std::uint64_t id);

private:
    void accept(const std::shared_ptr<Listener>& listener);
    // Closes the acceptors (on their strands) and waits for that unless on the reactor.
    void close_listeners(std::vector<std::shared_ptr<Listener>> listeners);

    std::shared_ptr<ReactorImpl> reactor_;

    mutable std::shared_mutex routes_mutex_;
    Router router_;
    HttpServer::Fallback fallback_;
    std::unordered_map<std::string, WsHandlers> websockets_;
    HttpServer::Handler websocket_fallback_;

    std::mutex listeners_mutex_;
    std::vector<std::shared_ptr<Listener>> listeners_;

    std::mutex sessions_mutex_;
    std::condition_variable sessions_changed_;
    std::unordered_map<std::uint64_t, std::weak_ptr<ServerConnection>> sessions_;
    std::unordered_map<std::uint64_t, std::weak_ptr<ServerConnection>> ws_sessions_;
    std::uint64_t next_session_ = 1;

    std::atomic<bool> stopping_{false};
};

} // namespace fv::net::detail
