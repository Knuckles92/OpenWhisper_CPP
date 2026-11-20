#include "appcore/preview_throttle.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>

namespace fv::appcore {

PreviewThrottle::PreviewThrottle(Clock& clock, Executor& ui, Publish publish)
    : clock_(clock), ui_(ui), publish_(std::move(publish)) {}

double PreviewThrottle::monotonic() const {
    // In double seconds, as Python computes, so the timer's whole milliseconds round the same.
    return std::chrono::duration<double>(clock_.steady().time_since_epoch()).count();
}

void PreviewThrottle::offer(std::string job, std::string text, bool final) {
    const double wait = sent_at_ ? *sent_at_ + kIntervalS - monotonic() : 0.0;
    if (final || wait <= 0) {
        timer_.cancel();
        held_.reset();
        send(std::move(job), std::move(text), final);
        return;
    }
    held_ = Held{std::move(job), std::move(text), final};
    if (!timer_.active()) {
        // max(1, int(wait * 1000) + 1): int() truncates, and wait is positive here.
        const auto ms = std::max<std::int64_t>(1, static_cast<std::int64_t>(wait * 1000) + 1);
        timer_ = ui_.post_after(std::chrono::milliseconds(ms),
                                [weak = std::weak_ptr<int>(alive_), this] {
                                    if (weak.lock()) {
                                        flush();
                                    }
                                });
    }
}

void PreviewThrottle::flush() {
    auto held = std::exchange(held_, std::nullopt);
    if (held) {
        send(std::move(held->job), std::move(held->text), held->final);
    }
}

void PreviewThrottle::send(std::string job, std::string text, bool final) {
    sent_at_ = monotonic();
    publish_(std::move(job), std::move(text), final);
}

} // namespace fv::appcore
