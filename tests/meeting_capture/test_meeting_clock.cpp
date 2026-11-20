// Ported from OpenWhisper tests/test_meeting_clock.py: pause credit, timestamp conversion and the
// recovery re-anchor, over a FakeClock whose steady time starts where the Python fixture's does.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "base/testing/fake_clock.h"
#include "capture_test_support.h"
#include "meeting_capture/meeting_clock.h"

using Catch::Approx;
using fv::FakeClock;
using fv::meeting::MeetingClock;
using fv::meeting::test::set_steady;

namespace {

struct Monotonic {
    Monotonic() { set_steady(clock, 10'000.0); }
    void advance(double seconds) {
        clock.advance(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(seconds)));
    }
    double value() const {
        return std::chrono::duration<double>(clock.steady().time_since_epoch()).count();
    }
    FakeClock clock;
};

} // namespace

// from tests/test_meeting_clock.py::TestMeetingClock::test_zero_before_start
TEST_CASE("zero before start", "[test_meeting_clock]") {
    Monotonic monotonic;
    MeetingClock clock(monotonic.clock);
    CHECK(clock.now_s() == 0.0);
    CHECK_FALSE(clock.is_running());
    CHECK(clock.meeting_time(monotonic.value()) == 0.0);
}

// from tests/test_meeting_clock.py::TestMeetingClock::test_advances_after_start
TEST_CASE("advances after start", "[test_meeting_clock]") {
    Monotonic monotonic;
    MeetingClock clock(monotonic.clock);
    clock.start();
    monotonic.advance(0.05);
    CHECK(clock.now_s() == Approx(0.05));
    CHECK(clock.is_running());
    REQUIRE(clock.started_at_iso());
    CHECK_FALSE(clock.started_at_iso()->empty());
}

// from tests/test_meeting_clock.py::TestMeetingClock::test_pause_freezes_meeting_time
TEST_CASE("pause freezes meeting time", "[test_meeting_clock]") {
    Monotonic monotonic;
    MeetingClock clock(monotonic.clock);
    clock.start();
    monotonic.advance(0.05);
    clock.pause();
    const double frozen = clock.now_s();
    monotonic.advance(0.08);
    CHECK(clock.now_s() == frozen);
    // Timestamps taken during the pause resolve to the pause instant.
    CHECK(clock.meeting_time(monotonic.value()) == frozen);
}

// from tests/test_meeting_clock.py::TestMeetingClock::test_resume_credits_paused_span
TEST_CASE("resume credits paused span", "[test_meeting_clock]") {
    Monotonic monotonic;
    MeetingClock clock(monotonic.clock);
    clock.start();
    monotonic.advance(0.05);
    const double before_pause = clock.now_s();
    clock.pause();
    monotonic.advance(0.1);
    clock.resume();
    const double after = clock.now_s();
    // Delays before the pause belong to meeting time; only the paused span disappears.
    CHECK(after == Approx(before_pause));
    CHECK(clock.paused_total_s() == Approx(0.1));
}

// from tests/test_meeting_clock.py::TestMeetingClock::test_pause_resume_idempotent
TEST_CASE("pause resume idempotent", "[test_meeting_clock]") {
    Monotonic monotonic;
    MeetingClock clock(monotonic.clock);
    clock.start();
    clock.resume(); // not paused: no-op
    clock.pause();
    clock.pause(); // double pause: no-op
    clock.resume();
    clock.resume(); // double resume: no-op
    CHECK(clock.is_running());
    CHECK_FALSE(clock.is_paused());
}

// from tests/test_meeting_clock.py::TestMeetingClock::test_recovery_reanchor_continues_timeline
TEST_CASE("recovery reanchor continues timeline", "[test_meeting_clock]") {
    Monotonic monotonic;
    MeetingClock clock(monotonic.clock);
    clock.resume_from_recovery(120.0);
    const double now = clock.now_s();
    CHECK(now == Approx(120.0));
    monotonic.advance(0.05);
    CHECK(clock.now_s() == Approx(now + 0.05));
}

TEST_CASE("a pause before start is ignored and the started time is kept on recovery",
          "[test_meeting_clock]") {
    Monotonic monotonic;
    MeetingClock clock(monotonic.clock);
    clock.pause();
    CHECK_FALSE(clock.is_paused());
    clock.start();
    const auto started = clock.started_at_iso();
    monotonic.advance(5.0);
    clock.resume_from_recovery(1.0);
    CHECK(clock.started_at_iso() == started);
    CHECK(clock.now_s() == Approx(1.0));
}
