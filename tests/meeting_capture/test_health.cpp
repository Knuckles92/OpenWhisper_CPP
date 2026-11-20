// Ported from OpenWhisper tests/test_meeting_capture_health.py: signal checks tell live quiet
// buffers from absent callbacks.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "meeting_capture/health.h"

using fv::meeting::CaptureBlock;
using fv::meeting::CaptureHealth;
using fv::meeting::CaptureSignalMonitor;

namespace {

// block(): 4000 samples of one value at 16 kHz.
struct Block {
    explicit Block(std::int16_t value, std::size_t samples = 4000) : data(samples, value) {}
    CaptureBlock view() const {
        return CaptureBlock{.channel = "mic",
                            .frames = data.data(),
                            .count = data.size(),
                            .rate = 16000,
                            .t_mono = 0.0};
    }
    std::vector<std::int16_t> data;
};

void observe(CaptureSignalMonitor& monitor, std::int16_t value, double now, bool signal = true) {
    const Block block(value);
    monitor.observe(block.view(), signal, now);
}

} // namespace

// from
// tests/test_meeting_capture_health.py::test_quiet_blocks_are_receiving_but_do_not_pass_guided_signal_check
TEST_CASE("quiet blocks are receiving but do not pass guided signal check",
          "[test_meeting_capture_health]") {
    CaptureSignalMonitor monitor;
    monitor.reset_stream(0.0);
    observe(monitor, 0, 0.25);
    CHECK(monitor.snapshot(0.5) ==
          CaptureHealth{.receiving = true, .stalled = false, .signal_windows = 0, .generation = 2});
}

// from
// tests/test_meeting_capture_health.py::test_attenuated_audio_does_not_pass_but_healthy_signal_does
TEST_CASE("attenuated audio does not pass but healthy signal does",
          "[test_meeting_capture_health]") {
    CaptureSignalMonitor monitor;
    monitor.reset_stream(0.0);
    observe(monitor, 50, 0.25); // 1% of a 5000-int16 voice window
    CHECK(monitor.snapshot(0.3).signal_windows == 0);
    for (double now : {0.5, 0.75, 1.0}) {
        observe(monitor, 5000, now);
    }
    CHECK(monitor.snapshot(1.1).signal_windows == 1);
}

// from
// tests/test_meeting_capture_health.py::test_short_click_is_not_enough_and_continuous_signal_is_rate_limited
TEST_CASE("short click is not enough and continuous signal is rate limited",
          "[test_meeting_capture_health]") {
    CaptureSignalMonitor monitor;
    monitor.reset_stream(0.0);
    const std::vector<std::pair<double, std::int16_t>> trace = {
        {0.25, 6000}, {0.5, 0}, {0.75, 6000}, {1.0, 6000}, {1.25, 6000}};
    for (const auto& [now, value] : trace) {
        observe(monitor, value, now);
    }
    CHECK(monitor.snapshot(1.3).signal_windows == 1);
    for (double now : {1.5, 1.75, 2.0, 2.25, 2.5, 2.75, 3.0, 3.25}) {
        observe(monitor, 6000, now);
    }
    CHECK(monitor.snapshot(3.3).signal_windows == 2);
}

// from tests/test_meeting_capture_health.py::test_wrong_source_at_same_level_can_pass_signal_check
TEST_CASE("wrong source at same level can pass signal check", "[test_meeting_capture_health]") {
    CaptureSignalMonitor monitor;
    for (double now : {1.0, 1.25, 1.5}) {
        observe(monitor, 3000, now);
    }
    // A limitation by design: amplitude alone cannot tell the intended microphone from the
    // system output.
    CHECK(monitor.snapshot(1.1).signal_windows == 1);
}

// from
// tests/test_meeting_capture_health.py::test_block_stall_and_source_restart_preserve_guided_check_counter
TEST_CASE("block stall and source restart preserve guided check counter",
          "[test_meeting_capture_health]") {
    CaptureSignalMonitor monitor;
    monitor.reset_stream(0.0);
    CHECK_FALSE(monitor.snapshot(3.9).stalled);
    CHECK(monitor.snapshot(4.0).stalled);
    for (double now : {4.1, 4.35, 4.6}) {
        observe(monitor, 1000, now);
    }
    CHECK(monitor.snapshot(4.2).signal_windows == 1);
    CHECK(monitor.snapshot(7.6).stalled);
    monitor.reset_stream(8.0);
    CHECK(
        monitor.snapshot(8.1) ==
        CaptureHealth{.receiving = false, .stalled = false, .signal_windows = 1, .generation = 3});
}

// from tests/test_meeting_capture_health.py::test_paused_capture_counts_buffers_but_not_signal
TEST_CASE("paused capture counts buffers but not signal", "[test_meeting_capture_health]") {
    CaptureSignalMonitor monitor;
    monitor.reset_stream(0.0);
    observe(monitor, 5000, 0.3, false);
    CHECK(monitor.snapshot(0.4) ==
          CaptureHealth{.receiving = true, .stalled = false, .signal_windows = 0, .generation = 2});
}

TEST_CASE("the snapshot serializes with Python's keys in order", "[test_meeting_capture_health]") {
    const CaptureHealth health{
        .receiving = true, .stalled = false, .signal_windows = 3, .generation = 2};
    CHECK(health.to_json().dump() ==
          R"({"receiving":true,"stalled":false,"signal_windows":3,"generation":2})");
}
