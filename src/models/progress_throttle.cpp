#include "models/progress_throttle.h"

#include <cmath>

namespace fv::models {

ProgressThrottle::ProgressThrottle(const Clock& clock, std::chrono::milliseconds interval)
    : clock_(clock), interval_(interval) {}

int ProgressThrottle::percent(std::int64_t done, std::int64_t total) {
    if (total <= 0) {
        return -1;
    }
    // Python's int(done * 100 / total): a true division truncated toward zero.
    return static_cast<int>(
        std::trunc(static_cast<double>(done) * 100.0 / static_cast<double>(total)));
}

bool ProgressThrottle::should_emit(std::int64_t done, std::int64_t total) {
    const auto now = clock_.steady();
    const int current = percent(done, total);
    const bool is_final = total > 0 && done >= total;
    // OpenWhisper starts the last emit at monotonic 0, so the first report always passes.
    if (last_emit_ && now - *last_emit_ < interval_ && current == last_percent_ && !is_final) {
        return false;
    }
    last_emit_ = now;
    last_percent_ = current;
    return true;
}

} // namespace fv::models
