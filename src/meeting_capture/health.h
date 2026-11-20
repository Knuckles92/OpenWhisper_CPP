#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

#include "base/clock.h"
#include "base/json.h"
#include "meeting_capture/capture_block.h"

// meeting/capture/health.py: per-channel health observations for live capture. An open stream is
// not proof that it delivers buffers or useful signal, so the monitor records block arrival and
// counts sustained signal windows. Quiet audio is never called faulty: a person or the system
// output may simply be silent. The signal counter lets the dashboard run a guided check.
namespace fv::meeting {

// The spool's int16 quiet threshold: no misses on 9,059 annotated 3-second speech-rich windows
// of ten AMI headset-mix meetings. A guided-check threshold, not a silence alarm.
inline constexpr double SIGNAL_RMS_INT16 = 300.0;
inline constexpr double SIGNAL_WINDOW_S = 0.25;
inline constexpr int MIN_CONSECUTIVE_SIGNAL_WINDOWS = 2;
inline constexpr double SIGNAL_REPORT_INTERVAL_S = 2.0;
inline constexpr double FIRST_BLOCK_GRACE_S = 4.0;
inline constexpr double BLOCK_STALL_S = 3.0;

struct CaptureHealth {
    bool receiving = false;
    bool stalled = false;
    std::int64_t signal_windows = 0;
    std::int64_t generation = 0;

    // {"receiving", "stalled", "signal_windows", "generation"}.
    Json to_json() const;
    friend bool operator==(const CaptureHealth&, const CaptureHealth&) = default;
};

// Times are monotonic seconds (MeetingClock::monotonic); a missing `now` reads the clock.
// Affinity: observe() on one device thread at a time (it touches only atomics and its own
// window, never allocates or locks); reset_stream() and snapshot() on any thread.
class CaptureSignalMonitor {
public:
    explicit CaptureSignalMonitor(const Clock& clock = system_clock(),
                                  double signal_rms = SIGNAL_RMS_INT16);
    CaptureSignalMonitor(const CaptureSignalMonitor&) = delete;
    CaptureSignalMonitor& operator=(const CaptureSignalMonitor&) = delete;

    // A new source generation; the signal counter is kept.
    void reset_stream(std::optional<double> now = std::nullopt);
    // Records the block's arrival and folds it into the current 0.25 s RMS window. While
    // signal_enabled is false (paused or not recording) the window and its run are cleared.
    void observe(const CaptureBlock& block, bool signal_enabled = true,
                 std::optional<double> now = std::nullopt) noexcept;
    CaptureHealth snapshot(std::optional<double> now = std::nullopt) const;

    std::int64_t signal_windows() const { return signal_windows_.load(); }

private:
    double now_or_clock(std::optional<double> now) const;

    const Clock& clock_;
    double signal_rms_;
    std::atomic<std::int64_t> generation_{0};
    std::atomic<double> started_mono_{0.0};
    std::atomic<double> last_block_mono_; // NaN until a block of this generation
    std::atomic<std::int64_t> signal_windows_{0};

    // The observing thread's window, cleared when it sees a new generation.
    std::int64_t seen_generation_ = -1;
    double sum_squares_ = 0.0;
    std::int64_t samples_ = 0;
    int sample_rate_ = 0;
    int consecutive_signal_windows_ = 0;
    std::optional<double> last_reported_signal_mono_;
};

} // namespace fv::meeting
