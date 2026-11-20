#pragma once

#include <functional>
#include <memory>
#include <string>

#include "hotkeys/debouncer.h"

namespace fv::hotkeys {

namespace detail {
struct DispatcherState;
} // namespace detail

// _hotkey_common.OrderedDispatcher: runs calls one at a time, in submission order, on one thread
// of its own named `name`, so hook threads only enqueue, a slow handler never delays the next
// key event's time stamp, and a release never overtakes its press.
//
// One deadline call may be pending (call_at replaces it). It runs once its time has come on the
// clock and every call queued before it has run, so a handler can settle a race between a
// deadline and an event by comparing the event's own time stamp. stop() ends the worker after the
// calls already queued; a later submit starts a new one. A call that throws is logged ("Hotkey
// handler failed") and the worker goes on.
// Affinity: thread-safe.
class OrderedDispatcher {
public:
    explicit OrderedDispatcher(std::string name, MonotonicClock clock = monotonic_clock());
    // Stops, then waits for the workers to finish what they had queued.
    ~OrderedDispatcher();
    OrderedDispatcher(const OrderedDispatcher&) = delete;
    OrderedDispatcher& operator=(const OrderedDispatcher&) = delete;

    void submit(std::function<void()> call);
    // Runs `call` at `when` (seconds on the clock), replacing a pending deadline.
    void call_at(double when, std::function<void()> call);
    void cancel_deadline();
    void stop();

private:
    std::shared_ptr<detail::DispatcherState> state_;
};

} // namespace fv::hotkeys
