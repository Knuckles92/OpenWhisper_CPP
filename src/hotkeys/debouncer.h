#pragma once

#include <functional>
#include <optional>

#include "base/clock.h"

namespace fv::hotkeys {

// Seconds on a monotonic clock, as Python's time.monotonic(): every hotkey time stamp and
// deadline is one, so the hold machine compares them exactly as the Python floats compare.
using MonotonicClock = std::function<double()>;

// The steady time of `clock` in seconds.
MonotonicClock monotonic_clock(const Clock& clock = system_clock());

// _hotkey_common.Debouncer: the first trigger passes, then one passes only when strictly more
// than the interval has gone by since the last one that passed. Monotonic, so wall-clock jumps do
// not matter.
// Affinity: not thread-safe; its owner serializes calls.
class Debouncer {
public:
    explicit Debouncer(int interval_ms, MonotonicClock clock = monotonic_clock());

    // True (and a new interval starts) when enough time has passed.
    bool should_trigger();
    // The same at a time the caller already read (the hook's time stamp of the event).
    bool should_trigger_at(double now);
    // The next trigger passes at once.
    void reset();

    int interval_ms() const { return interval_ms_; }
    std::optional<double> last_trigger_time() const { return last_; }

private:
    int interval_ms_;
    MonotonicClock clock_;
    std::optional<double> last_;
};

} // namespace fv::hotkeys
