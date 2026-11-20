#include "log/startup_profiler.h"

#include <format>

namespace fv::log {

namespace {

// "%-32s": padded to 32 code points, as Python counts str length.
std::string pad_right(std::string_view text, std::size_t width) {
    std::size_t length = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++length;
        }
    }
    std::string padded(text);
    if (length < width) {
        padded.append(width - length, ' ');
    }
    return padded;
}

} // namespace

StartupProfiler::StartupProfiler() : StartupProfiler(system_clock()) {}

StartupProfiler::StartupProfiler(const Clock& clock) : clock_(&clock), start_(clock.steady()) {}

StartupProfiler::StartupProfiler(const Clock& clock, Events events)
    : clock_(&clock), start_(clock.steady()), events_(std::move(events)) {}

void StartupProfiler::mark(std::string_view name) {
    const std::chrono::duration<double> elapsed = clock_->steady() - start_;
    events_.emplace_back(std::string(name), elapsed.count());
}

void StartupProfiler::log_summary(const Logger& logger) const {
    logger.info("Startup timing summary:");
    double previous = 0.0;
    for (const auto& [name, elapsed] : events_) {
        logger.info("  {} total={:7.3f}s delta={:7.3f}s", pad_right(name, 32), elapsed,
                    elapsed - previous);
        previous = elapsed;
    }
}

const StartupProfiler::Events& StartupProfiler::events() const {
    return events_;
}

} // namespace fv::log
