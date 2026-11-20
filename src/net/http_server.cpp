#include "net/http_server.h"

#include <future>
#include <utility>

#include "net/detail/platform.h"
#include "net/detail/server_impl.h"
#include "net/detail/server_log.h"
#include "net/detail/session.h"
#include "net/detail/tls_context.h"
#include "net/detail/ws_session.h"

namespace fv::net {

namespace detail {

namespace {

// The backlog OpenWhisper's MCP runtime listens with (runtime.py _bind_sockets).
constexpr int kBacklog = 128;
// How long stop() and a failed listen() wait for the reactor to close sockets.
constexpr std::chrono::seconds kCloseWait{2};
// After the grace, how long stop() waits for the forced closes to run.
constexpr std::chrono::seconds kForceWait{1};
// After a failed accept, before the next (asyncio's ACCEPT_RETRY_DELAY).
constexpr std::chrono::seconds kAcceptRetry{1};

std::string describe(const Listen& where) {
    const bool v6 = where.address.find(':') != std::string::npos;
    return (v6 ? "[" + where.address + "]" : where.address) + ":" + std::to_string(where.port);
}

} // namespace

ServerImpl::ServerImpl(std::shared_ptr<ReactorImpl> reactor) : reactor_(std::move(reactor)) {}

void ServerImpl::add_route(std::string method, std::string_view pattern, RouteOptions options,
                           HttpServer::Handler handler) {
    RoutePattern compiled(pattern); // throws before anything changes
    std::unique_lock lock(routes_mutex_);
    router_.add(Route{std::move(method), std::move(compiled), options, std::move(handler)});
}

void ServerImpl::set_fallback(HttpServer::Fallback fallback) {
    std::unique_lock lock(routes_mutex_);
    fallback_ = std::move(fallback);
}

Dispatch ServerImpl::dispatch_for(std::string_view method, std::string_view path) const {
    std::shared_lock lock(routes_mutex_);
    RouteMatch match = router_.match(method, path);
    Dispatch dispatch;
    if (match.route != nullptr) {
        dispatch.handler = match.route->handler;
        dispatch.options = match.route->options;
        dispatch.params = std::move(match.params);
        return dispatch;
    }
    dispatch.path_known = match.path_known;
    dispatch.allowed = std::move(match.allowed);
    dispatch.redirect_path = std::move(match.redirect_path);
    if (!dispatch.redirect_path) {
        dispatch.fallback = fallback_;
    }
    return dispatch;
}

void ServerImpl::add_websocket(std::string path, WsHandlers handlers) {
    std::unique_lock lock(routes_mutex_);
    websockets_.insert_or_assign(std::move(path), std::move(handlers));
}

std::optional<WsHandlers> ServerImpl::websocket_for(const std::string& path) const {
    std::shared_lock lock(routes_mutex_);
    const auto found = websockets_.find(path);
    if (found == websockets_.end()) {
        return std::nullopt;
    }
    return found->second;
}

void ServerImpl::set_websocket_fallback(HttpServer::Handler fallback) {
    std::unique_lock lock(routes_mutex_);
    websocket_fallback_ = std::move(fallback);
}

HttpServer::Handler ServerImpl::websocket_fallback() const {
    std::shared_lock lock(routes_mutex_);
    return websocket_fallback_;
}

std::optional<std::chrono::milliseconds> ServerImpl::open_deadline() const {
    std::shared_lock lock(routes_mutex_);
    std::optional<std::chrono::milliseconds> shortest;
    for (const auto& [path, handlers] : websockets_) {
        if (handlers.open_timeout.count() > 0 && (!shortest || handlers.open_timeout < *shortest)) {
            shortest = handlers.open_timeout;
        }
    }
    return shortest;
}

void ServerImpl::close_listeners(std::vector<std::shared_ptr<Listener>> listeners) {
    if (listeners.empty()) {
        return;
    }
    if (reactor_->stopped()) {
        // No thread runs the acceptors any more, so nothing else touches them.
        for (const auto& listener : listeners) {
            boost::system::error_code ignored;
            listener->retry.cancel();
            listener->acceptor.close(ignored);
        }
        return;
    }
    std::vector<std::future<void>> closed;
    for (const auto& listener : listeners) {
        auto done = std::make_shared<std::promise<void>>();
        closed.push_back(done->get_future());
        asio::post(listener->acceptor.get_executor(), [listener, done] {
            boost::system::error_code ignored;
            listener->retry.cancel();
            listener->acceptor.close(ignored);
            done->set_value();
        });
    }
    if (reactor_->running_in_this_thread()) {
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + kCloseWait;
    for (auto& future : closed) {
        future.wait_until(deadline);
    }
}

void ServerImpl::accept(const std::shared_ptr<Listener>& listener) {
    listener->acceptor.async_accept(
        asio::make_strand(reactor_->io()),
        [weak = weak_from_this(), listener](const boost::system::error_code& error,
                                            tcp::socket socket) {
            auto self = weak.lock();
            if (!self || !listener->acceptor.is_open() || error == asio::error::operation_aborted) {
                return;
            }
            if (error) {
                // Out of sockets or memory: asyncio logs it and accepts again after
                // ACCEPT_RETRY_DELAY, rather than spinning on the same error.
                log_connection_error(ErrorSite::Other, false, error.message());
                listener->retry.expires_after(kAcceptRetry);
                listener->retry.async_wait(
                    [weak, listener](const boost::system::error_code& waited) {
                        auto server = weak.lock();
                        if (!waited && server && listener->acceptor.is_open()) {
                            server->accept(listener);
                        }
                    });
                return;
            }
            if (self->stopping()) {
                boost::system::error_code ignored;
                socket.close(ignored);
            } else {
                const auto accepted = std::chrono::steady_clock::now();
                std::uint64_t id = 0;
                {
                    std::lock_guard lock(self->sessions_mutex_);
                    id = self->next_session_++;
                }
                // Started outside the lock: a session dropped at once unregisters under it.
                const auto open = [&](auto session) {
                    {
                        std::lock_guard lock(self->sessions_mutex_);
                        self->sessions_[id] = session;
                    }
                    session->start();
                };
                if (listener->tls) {
                    open(std::make_shared<Session<TlsStream>>(
                        id, self, TlsStream(PlainStream(std::move(socket)), *listener->tls),
                        accepted));
                } else {
                    open(std::make_shared<Session<PlainStream>>(
                        id, self, PlainStream(std::move(socket)), accepted));
                }
            }
            self->accept(listener);
        });
}

std::expected<Endpoint, std::string> ServerImpl::listen(const Listen& where) {
    std::lock_guard lock(listeners_mutex_);
    const auto fail = [&](const std::string& reason) -> std::expected<Endpoint, std::string> {
        // A start that binds several addresses must not leave the first ones bound.
        close_listeners(std::exchange(listeners_, {}));
        return std::unexpected("Could not listen on " + describe(where) + ": " + reason);
    };
    if (stopping_) {
        return fail("the server is stopped");
    }
    boost::system::error_code error;
    const auto address = asio::ip::make_address(where.address, error);
    if (error) {
        return fail("not an IP address");
    }
    auto listener = std::make_shared<Listener>(reactor_->io());
    if (where.tls) {
        auto context = make_server_context(*where.tls);
        if (!context) {
            return fail(context.error());
        }
        listener->tls = std::move(*context);
    }
    const tcp::endpoint endpoint(address, where.port);
    listener->acceptor.open(endpoint.protocol(), error);
    if (error) {
        return fail(error.message());
    }
    if (const auto reuse = set_address_reuse(
            static_cast<std::uintptr_t>(listener->acceptor.native_handle()), where.exclusive)) {
        return fail(reuse.message());
    }
    if (address.is_v6()) {
        listener->acceptor.set_option(asio::ip::v6_only(true), error);
    }
    listener->acceptor.bind(endpoint, error);
    if (error) {
        return fail(error.message());
    }
    listener->acceptor.listen(kBacklog, error);
    if (error) {
        return fail(error.message());
    }
    const auto local = listener->acceptor.local_endpoint(error);
    if (error) {
        return fail(error.message());
    }
    listeners_.push_back(listener);
    asio::post(listener->acceptor.get_executor(),
               [self = shared_from_this(), listener] { self->accept(listener); });
    return Endpoint{local.address().to_string(), local.port()};
}

void ServerImpl::stop(std::chrono::milliseconds grace) {
    stopping_ = true;
    {
        std::lock_guard lock(listeners_mutex_);
        close_listeners(std::exchange(listeners_, {}));
    }
    const auto live_sessions = [this] {
        std::vector<std::shared_ptr<ServerConnection>> live;
        std::lock_guard lock(sessions_mutex_);
        for (const auto& [id, weak] : sessions_) {
            if (auto session = weak.lock()) {
                live.push_back(std::move(session));
            }
        }
        return live;
    };
    const auto live_websockets = [this] {
        std::vector<std::shared_ptr<ServerConnection>> live;
        std::lock_guard lock(sessions_mutex_);
        for (const auto& [id, weak] : ws_sessions_) {
            if (auto session = weak.lock()) {
                live.push_back(std::move(session));
            }
        }
        return live;
    };
    const auto all_closed = [this] {
        return sessions_.empty() && ws_sessions_.empty();
    };
    for (const auto& session : live_sessions()) {
        session->server_stopping();
    }
    // uvicorn's shutdown sends every open WebSocket its close frame at once.
    for (const auto& session : live_websockets()) {
        session->server_stopping();
    }
    const bool can_wait = !reactor_->running_in_this_thread();
    if (can_wait) {
        std::unique_lock lock(sessions_mutex_);
        sessions_changed_.wait_for(lock, grace, all_closed);
    }
    for (const auto& session : live_sessions()) {
        session->force_close();
    }
    for (const auto& session : live_websockets()) {
        session->force_close();
    }
    if (can_wait) {
        std::unique_lock lock(sessions_mutex_);
        sessions_changed_.wait_for(lock, kForceWait, all_closed);
    }
}

void ServerImpl::session_closed(std::uint64_t id) {
    {
        std::lock_guard lock(sessions_mutex_);
        sessions_.erase(id);
    }
    sessions_changed_.notify_all();
}

void ServerImpl::ws_opened(std::uint64_t id, const std::shared_ptr<ServerConnection>& session) {
    std::lock_guard lock(sessions_mutex_);
    ws_sessions_[id] = session;
}

void ServerImpl::ws_closed(std::uint64_t id) {
    {
        std::lock_guard lock(sessions_mutex_);
        ws_sessions_.erase(id);
    }
    sessions_changed_.notify_all();
}

} // namespace detail

std::unique_ptr<HttpServer> HttpServer::create(Reactor& reactor) {
    return std::unique_ptr<HttpServer>(
        new HttpServer(std::make_shared<detail::ServerImpl>(reactor.impl_)));
}

HttpServer::HttpServer(std::shared_ptr<detail::ServerImpl> impl) : impl_(std::move(impl)) {}

HttpServer::~HttpServer() {
    if (!impl_->stopping()) {
        impl_->stop(std::chrono::milliseconds(0));
    }
}

void HttpServer::route(
    std::string method, std::string pattern,
    std::function<void(const HttpRequest&, std::shared_ptr<Responder>)> handler) {
    route(std::move(method), std::move(pattern), RouteOptions{}, std::move(handler));
}

void HttpServer::route(
    std::string method, std::string pattern, RouteOptions options,
    std::function<void(const HttpRequest&, std::shared_ptr<Responder>)> handler) {
    impl_->add_route(std::move(method), pattern, options, std::move(handler));
}

void HttpServer::set_fallback(
    std::function<void(const HttpRequest&, bool path_known, std::shared_ptr<Responder>)> fallback) {
    impl_->set_fallback(std::move(fallback));
}

void HttpServer::websocket(std::string path, WsHandlers handlers) {
    impl_->add_websocket(std::move(path), std::move(handlers));
}

void HttpServer::set_websocket_fallback(
    std::function<void(const HttpRequest&, std::shared_ptr<Responder>)> fallback) {
    impl_->set_websocket_fallback(std::move(fallback));
}

std::expected<Endpoint, std::string> HttpServer::listen(const Listen& where) {
    return impl_->listen(where);
}

void HttpServer::stop(std::chrono::milliseconds grace) {
    impl_->stop(grace);
}

} // namespace fv::net
