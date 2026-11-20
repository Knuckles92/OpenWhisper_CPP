#include "hotkeys/debouncer.h"

#include <chrono>
#include <utility>

namespace fv::hotkeys {

MonotonicClock monotonic_clock(const Clock& clock) {
    return [&clock] {
        return std::chrono::duration<double>(clock.steady().time_since_epoch()).count();
    };
}

Debouncer::Debouncer(int interval_ms, MonotonicClock clock)
    : interval_ms_(interval_ms), clock_(std::move(clock)) {}

bool Debouncer::should_trigger() {
    return should_trigger_at(clock_());
}

bool Debouncer::should_trigger_at(double now) {
    if (!last_) {
        last_ = now;
        return true;
    }
    // Python compares against interval_ms / 1000.0; the same division keeps the same edge.
    if (now - *last_ > (interval_ms_ / 1000.0)) {
        last_ = now;
        return true;
    }
    return false;
}

void Debouncer::reset() {
    last_.reset();
}

} // namespace fv::hotkeys
