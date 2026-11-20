#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "base/clock.h"
#include "base/executor.h"

namespace fv::appcore {

// Live preview text as transcription.preview events, at most four a second (OpenWhisper's
// PreviewEvents). Text that arrives too soon is held and sent when the quarter second is up (only
// the newest), so listeners always end on the latest preview. A final preview goes out at once
// and drops the held text. The publisher drops text held, or still on its way, when its
// recording stops or is canceled.
// Affinity: UI actor on the executor it is given.
class PreviewThrottle {
public:
    static constexpr double kIntervalS = 0.25; // INTERVAL_S, seconds

    using Publish = std::function<void(std::string job, std::string text, bool final)>;

    PreviewThrottle(Clock& clock, Executor& ui, Publish publish);
    PreviewThrottle(const PreviewThrottle&) = delete;
    PreviewThrottle& operator=(const PreviewThrottle&) = delete;

    void offer(std::string job, std::string text, bool final);

private:
    struct Held {
        std::string job;
        std::string text;
        bool final;
    };

    void flush();
    void send(std::string job, std::string text, bool final);
    double monotonic() const; // seconds on the steady clock, as Python's time.monotonic()

    Clock& clock_;
    Executor& ui_;
    Publish publish_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    TimerHandle timer_;
    std::optional<double> sent_at_;
    std::optional<Held> held_;
};

} // namespace fv::appcore
