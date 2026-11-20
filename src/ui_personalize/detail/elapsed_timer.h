#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "base/clock.h"

namespace fv::detail {

// QElapsedTimer on an injectable clock: whole milliseconds since start(), as QElapsedTimer
// counts them, so a test moves time with a FakeClock instead of waiting for it.
// Affinity: GUI thread.
class ElapsedTimer {
public:
    explicit ElapsedTimer(const Clock* clock)
        : clock_(clock != nullptr ? clock : &system_clock()) {}

    void start() { started_ = clock_->steady(); }
    void restart() { start(); }
    bool isValid() const { return started_.has_value(); }
    // Milliseconds since start(); 0 before it.
    std::int64_t elapsed() const {
        if (!started_) {
            return 0;
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock_->steady() - *started_)
            .count();
    }

private:
    const Clock* clock_;
    std::optional<std::chrono::steady_clock::time_point> started_;
};

} // namespace fv::detail
