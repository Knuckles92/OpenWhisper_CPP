#pragma once

#include <chrono>
#include <memory>

#include "base/executor.h"
#include "base/subscription.h"

namespace fv {

namespace detail {
struct CancelState;
} // namespace detail

// Every wait takes one; shutdown cancels the root source first. A default-constructed token is
// never cancelled. Copies share the state of their source.
// Affinity: thread-safe.
class CancelToken {
public:
    CancelToken() = default;

    bool cancelled() const;
    // Blocks until cancelled or the time is up; a token that can never be cancelled simply
    // sleeps.
    bool wait_for(std::chrono::milliseconds) const; // true when cancelled
    // When the token is already cancelled the task runs at once, on the calling thread, and the
    // subscription returned is empty. A task that throws is logged ("base.cancel").
    Subscription on_cancel(Task) const; // runs once, on the cancelling thread

private:
    friend class CancelSource;
    explicit CancelToken(std::shared_ptr<detail::CancelState> state);
    std::shared_ptr<detail::CancelState> state_;
};

// Copies share one state, as std::stop_source's do.
// Affinity: thread-safe.
class CancelSource {
public:
    CancelSource();
    // A child: cancelled when `parent` is (at once if it already is), and on its own.
    explicit CancelSource(const CancelToken& parent);

    CancelToken token() const;
    void cancel();
    bool cancelled() const;

private:
    std::shared_ptr<detail::CancelState> state_;
};

} // namespace fv
