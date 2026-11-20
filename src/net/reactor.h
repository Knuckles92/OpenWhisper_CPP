#pragma once

#include <chrono>
#include <memory>

#include "base/executor.h"

namespace fv::net {

namespace detail {
class ReactorImpl;
struct ReactorAccess;
} // namespace detail

// Owns the asio io_context, threads "fv-net-0", "fv-net-1". Every server and WebSocket client
// runs on it.
// Affinity: thread-safe; stop() and the destructor are not called from the reactor's own threads.
class Reactor {
public:
    explicit Reactor(int threads = 2);
    // stop(0) when stop() was not called. Work posted afterwards is dropped.
    ~Reactor();
    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    Executor& executor(); // handlers run here and never block
    // Lets queued work and pending timers run for at most `grace`, then ends the threads. Servers
    // are stopped first (HttpServer::stop); a later post is dropped.
    void stop(std::chrono::milliseconds grace);

private:
    friend class HttpServer;
    friend struct detail::ReactorAccess; // the WebSocket client
    std::shared_ptr<detail::ReactorImpl> impl_;
};

} // namespace fv::net
