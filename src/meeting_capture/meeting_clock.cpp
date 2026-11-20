#include "meeting_capture/meeting_clock.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include "meeting_state/time_utils.h"

namespace fv::meeting {

namespace {

constexpr double kNone = std::numeric_limits<double>::quiet_NaN();

} // namespace

MeetingClock::MeetingClock(const Clock& clock) : clock_(clock), t0_(kNone), paused_at_(kNone) {}

MeetingClock::State MeetingClock::read() const {
    while (true) {
        const std::uint64_t before = sequence_.load(std::memory_order_acquire);
        if ((before & 1U) != 0) {
            continue;
        }
        const State state{t0_.load(std::memory_order_relaxed),
                          credit_.load(std::memory_order_relaxed),
                          paused_at_.load(std::memory_order_relaxed)};
        std::atomic_thread_fence(std::memory_order_acquire);
        if (sequence_.load(std::memory_order_relaxed) == before) {
            return state;
        }
    }
}

void MeetingClock::write(double t0, double credit, double paused_at) {
    // Called with mutex_ held, so there is one writer at a time.
    const std::uint64_t sequence = sequence_.load(std::memory_order_relaxed);
    sequence_.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    t0_.store(t0, std::memory_order_relaxed);
    credit_.store(credit, std::memory_order_relaxed);
    paused_at_.store(paused_at, std::memory_order_relaxed);
    sequence_.store(sequence + 2, std::memory_order_release);
}

double MeetingClock::monotonic() const {
    return std::chrono::duration<double>(clock_.steady().time_since_epoch()).count();
}

void MeetingClock::start() {
    const std::lock_guard lock(mutex_);
    write(monotonic(), 0.0, kNone);
    started_at_iso_ = meeting_state::utc_now_iso(clock_);
}

void MeetingClock::resume_from_recovery(double elapsed_s) {
    const std::lock_guard lock(mutex_);
    write(monotonic() - elapsed_s, 0.0, kNone);
    if (!started_at_iso_) {
        started_at_iso_ = meeting_state::utc_now_iso(clock_);
    }
}

void MeetingClock::pause() {
    const std::lock_guard lock(mutex_);
    const State state = read();
    if (!std::isnan(state.t0) && std::isnan(state.paused_at)) {
        write(state.t0, state.credit, monotonic());
    }
}

void MeetingClock::resume() {
    const std::lock_guard lock(mutex_);
    const State state = read();
    if (!std::isnan(state.paused_at)) {
        write(state.t0, state.credit + (monotonic() - state.paused_at), kNone);
    }
}

bool MeetingClock::is_paused() const {
    return !std::isnan(paused_at_.load(std::memory_order_acquire));
}

bool MeetingClock::is_running() const {
    return !std::isnan(t0_.load(std::memory_order_acquire));
}

double MeetingClock::now_s() const {
    return meeting_time(monotonic());
}

double MeetingClock::meeting_time(double t_mono) const {
    const State state = read();
    if (std::isnan(state.t0)) {
        return 0.0;
    }
    // Python's min() keeps its first argument on a tie.
    const double effective = std::isnan(state.paused_at)
                                 ? t_mono
                                 : (state.paused_at < t_mono ? state.paused_at : t_mono);
    return std::max(0.0, effective - state.t0 - state.credit);
}

double MeetingClock::paused_total_s() const {
    const State state = read();
    double credit = state.credit;
    if (!std::isnan(state.paused_at)) {
        credit += monotonic() - state.paused_at;
    }
    return credit;
}

std::optional<std::string> MeetingClock::started_at_iso() const {
    const std::lock_guard lock(mutex_);
    return started_at_iso_;
}

} // namespace fv::meeting
