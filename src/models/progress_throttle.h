#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "base/clock.h"

// The byte-progress throttle of model downloads and component installs
// (services/runtime/models.py:_hf_download_progress, services/runtime/components.py:131-141): a
// multi-gigabyte download in 1 MiB chunks would otherwise queue thousands of cross-thread events
// and stall the UI. Progress passes at most once per interval, or when its whole percent changes,
// or when it is final.
namespace fv::models {

// Affinity: one download's thread (not thread-safe).
class ProgressThrottle {
public:
    explicit ProgressThrottle(const Clock& clock = system_clock(),
                              std::chrono::milliseconds interval = std::chrono::milliseconds(100));

    // Whether (done, total) bytes should be reported now; remembers it when so. The percent is
    // int(done * 100 / total), or -1 for an unknown total; final means done >= total > 0.
    bool should_emit(std::int64_t done, std::int64_t total);

    // int(done * 100 / total) for total > 0, else -1.
    static int percent(std::int64_t done, std::int64_t total);

private:
    const Clock& clock_;
    std::chrono::milliseconds interval_;
    std::optional<std::chrono::steady_clock::time_point> last_emit_;
    int last_percent_ = -1;
};

} // namespace fv::models
