#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "base/clock.h"

// meeting/clock.py: the pausable monotonic timeline both capture streams share. Every timestamp
// in Meeting Mode (chunk offsets, segment times, evidence anchors) is in meeting seconds: seconds
// since the meeting started, excluding paused time. Monotonic seconds are the injected clock's
// steady time, so the timeline ignores wall-clock changes.
namespace fv::meeting {

// Affinity: thread-safe. Readers (is_paused, meeting_time, now_s) take no lock, so the device
// callbacks call them; start, pause and resume serialize on a mutex.
class MeetingClock {
public:
    explicit MeetingClock(const Clock& clock = system_clock());
    MeetingClock(const MeetingClock&) = delete;
    MeetingClock& operator=(const MeetingClock&) = delete;

    // Anchors the meeting epoch at the current monotonic instant.
    void start();
    // Re-anchors after crash recovery so meeting time continues at `elapsed_s`; the downtime is
    // not counted. started_at_iso is set only when it was not.
    void resume_from_recovery(double elapsed_s);
    // Freezes meeting time; idempotent, and a no-op before start.
    void pause();
    // Unfreezes meeting time, crediting the paused span; idempotent.
    void resume();

    bool is_paused() const;
    bool is_running() const;

    // Current meeting time in seconds (0.0 before start).
    double now_s() const;
    // A monotonic timestamp in meeting seconds. Timestamps taken while paused resolve to the
    // pause instant, so blocks that race a pause land at a stable position; never below 0.
    double meeting_time(double t_mono) const;
    // Pause credit so far, the running pause included.
    double paused_total_s() const;
    // UTC ISO text of the last start(), or of the first recovery when start() never ran.
    std::optional<std::string> started_at_iso() const;

    // The injected clock's steady time in seconds: what t_mono values are measured in.
    double monotonic() const;
    const Clock& clock() const { return clock_; }

private:
    struct State {
        double t0;
        double credit;
        double paused_at;
    };
    State read() const;
    void write(double t0, double credit, double paused_at);

    const Clock& clock_;
    mutable std::mutex mutex_; // writers, and started_at_iso_
    // A sequence lock: odd while a writer is between its two increments.
    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<double> t0_; // NaN until started
    std::atomic<double> credit_{0.0};
    std::atomic<double> paused_at_;             // NaN while running
    std::optional<std::string> started_at_iso_; // guarded by mutex_
};

} // namespace fv::meeting
