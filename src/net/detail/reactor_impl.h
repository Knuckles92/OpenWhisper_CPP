#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "base/executor.h"
#include "net/detail/asio.h"
#include "net/reactor.h"

namespace fv::net::detail {

class ReactorImpl;

// Executor over the io_context: tasks and timers run on the reactor's threads.
class ReactorExecutor final : public Executor {
public:
    explicit ReactorExecutor(ReactorImpl& reactor) : reactor_(reactor) {}
    void post(Task) override;
    TimerHandle post_after(std::chrono::milliseconds, Task) override;
    bool is_current() const override;

private:
    ReactorImpl& reactor_;
};

// An io_context whose pending handlers can be destroyed before the object is: a handler that
// holds a connection, which holds the context, would otherwise keep both alive forever.
class IoContext : public asio::io_context {
public:
    using asio::execution_context::shutdown;
    using asio::io_context::io_context;
};

// What Reactor holds. Shared, because connections keep the io_context alive until their last
// owner (a Responder held by some library) lets go.
// Affinity: thread-safe.
class ReactorImpl : public std::enable_shared_from_this<ReactorImpl> {
public:
    explicit ReactorImpl(int threads);
    ~ReactorImpl();
    ReactorImpl(const ReactorImpl&) = delete;
    ReactorImpl& operator=(const ReactorImpl&) = delete;

    asio::io_context& io() { return io_; }
    Executor& executor() { return executor_; }
    // True on a thread that is running the io_context.
    bool running_in_this_thread();
    // Once true, posts are dropped.
    bool stopped() const { return stopped_.load(); }
    void stop(std::chrono::milliseconds grace);
    // After stop(): destroys every handler still queued (Reactor's destructor).
    void discard_pending();

private:
    void run(int index);

    IoContext io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    ReactorExecutor executor_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stopped_{false};

    std::mutex mutex_;
    std::condition_variable idle_;
    int running_ = 0;
    bool joined_ = false;
};

// What fv_net's own sources reach inside a Reactor.
struct ReactorAccess {
    static const std::shared_ptr<ReactorImpl>& impl(const Reactor& reactor) {
        return reactor.impl_;
    }
};

} // namespace fv::net::detail
