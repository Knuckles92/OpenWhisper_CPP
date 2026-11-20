#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace fv {

// Gates the runs of one callback, so that ending a connection is final: once close() returns,
// no run starts, and no run is still going on another thread. A run on the thread that calls
// close() (a callback ending its own connection) is not waited for. So a callback may capture
// `this` of the object that holds its Subscription, but it must not wait for the thread that
// destroys that object.
// Affinity: thread-safe.
class CallbackGuard {
public:
    // Runs fn unless the guard is closed; true when it ran.
    template <class Fn> bool run(Fn&& fn) {
        if (!enter()) {
            return false;
        }
        struct Leave {
            CallbackGuard* guard;
            ~Leave() { guard->leave(); }
        } leave{this};
        std::forward<Fn>(fn)();
        return true;
    }

    void close();
    bool closed() const;

private:
    bool enter();
    void leave();

    mutable std::mutex mutex_;
    std::condition_variable idle_;
    std::vector<std::thread::id> running_;
    bool closed_ = false;
};

// A connection to a Signal, a settings watch or a cancel callback. Destroying it, or calling
// disconnect(), ends the connection; afterwards the callback is never started again. Move-only.
// Affinity: thread-safe.
class Subscription {
public:
    Subscription() = default; // connected to nothing
    // For sources: disconnect() closes `guard` (which the source runs the callback through),
    // then calls `unlink` once to drop the callback from the source. `unlink` must cope with a
    // source that no longer exists.
    Subscription(std::shared_ptr<CallbackGuard> guard, std::function<void()> unlink);
    Subscription(Subscription&&) noexcept;
    Subscription& operator=(Subscription&&) noexcept;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    ~Subscription();

    void disconnect();
    // False once disconnected, and once the source has ended the connection itself (a cancel
    // callback after it ran).
    bool connected() const;

private:
    std::shared_ptr<CallbackGuard> guard_;
    std::function<void()> unlink_;
};

} // namespace fv
