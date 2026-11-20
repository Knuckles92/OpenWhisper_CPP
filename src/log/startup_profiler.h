#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/log.h"

namespace fv::log {

// ui_qt/startup_profiler.py's logger.
inline constexpr Logger kStartupProfilerLogger{"ui_qt.startup_profiler"};

// Named startup marks with the seconds since the profiler was made, and a log summary of totals
// and deltas.
// Affinity: one thread at a time (the startup sequence).
class StartupProfiler {
public:
    using Events = std::vector<std::pair<std::string, double>>; // name, seconds since start

    StartupProfiler();
    explicit StartupProfiler(const Clock& clock);
    // A profiler holding `events` already (StartupProfiler(start_time=..., events=...)).
    StartupProfiler(const Clock& clock, Events events);

    void mark(std::string_view name);
    // "Startup timing summary:", then "  %-32s total=%7.3fs delta=%7.3fs" per mark, at INFO.
    void log_summary(const Logger& logger = kStartupProfilerLogger) const;
    const Events& events() const;

private:
    const Clock* clock_;
    std::chrono::steady_clock::time_point start_;
    Events events_;
};

} // namespace fv::log
