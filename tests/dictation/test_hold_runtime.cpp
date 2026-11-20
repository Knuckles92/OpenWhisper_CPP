#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <thread>

#include "dictation_support.h"
#include "hotkeys/hold_machine.h"
#include "hotkeys/matcher.h"

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::hotkeys::HoldMachine;
using fv::hotkeys::TriggerMode;

namespace {

using Calls = std::vector<std::string>;

// config.RECORD_LATCH_WINDOW_MS in seconds.
constexpr double kWindow = fv::hotkeys::kRecordLatchWindowMs / 1000.0;

void sleep_s(double seconds) {
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

// test_push_hold_recording._FakeController: a stop or cancel leaves the recorder as it was.
void as_push_hold_controller(Rig& rig) {
    rig.control.ends_recordings = false;
}

} // namespace

// ---- tests/test_personalize_fix_f1_latch_no_data.py ----
// The stream reports recording as soon as it opens, but the first audio block can take a while,
// so a quick tap's release often finds no audio yet. That is not a cancel. (The C++ fake has no
// audio blocks: the runtime never asks for them, as Python's no longer does.)

// from
// tests/test_personalize_fix_f1_latch_no_data.py::test_a_double_tap_before_any_audio_latches_and_the_next_press_stops
TEST_CASE("test_a_double_tap_before_any_audio_latches_and_the_next_press_stops",
          "[test_personalize_fix_f1_latch_no_data]") {
    Rig rig;
    const double pressed_at = rig.tap_then_press(0.15);
    rig.runtime->queue_record_release(pressed_at + 0.08);
    rig.drain();

    CHECK(rig.hands_free->values() == std::vector<bool>{true});
    CHECK(rig.control.calls() == Calls{"start"});

    rig.runtime->queue_record_press(pressed_at + 2);
    rig.drain();
    CHECK(rig.control.calls() == Calls{"start", "stop"});
    CHECK(rig.hands_free->values() == std::vector<bool>{true, false});
}

// from
// tests/test_personalize_fix_f1_latch_no_data.py::test_a_lone_tap_before_any_audio_still_cancels_after_the_window
TEST_CASE("test_a_lone_tap_before_any_audio_still_cancels_after_the_window",
          "[test_personalize_fix_f1_latch_no_data]") {
    Rig rig;
    const double start = monotonic_now();
    rig.runtime->queue_record_press(start);
    rig.runtime->queue_record_release(start + 0.05);

    CHECK(rig.control.wait_for_calls(
        2, std::chrono::milliseconds(static_cast<int>((kWindow + 2) * 1000))));
    CHECK(rig.control.calls() == Calls{"start", "cancel"});
}

// from
// tests/test_personalize_fix_f1_latch_no_data.py::test_without_the_latch_a_tap_before_any_audio_cancels_at_once
TEST_CASE("test_without_the_latch_a_tap_before_any_audio_cancels_at_once",
          "[test_personalize_fix_f1_latch_no_data]") {
    Rig rig;
    rig.set_latch(false);
    const double start = monotonic_now();
    rig.runtime->queue_record_press(start);
    rig.runtime->queue_record_release(start + 0.05);
    rig.drain();

    CHECK(rig.control.calls() == Calls{"start", "cancel"});
}

// from
// tests/test_personalize_fix_f1_latch_no_data.py::test_a_long_hold_without_audio_stops_instead_of_leaving_the_mic_open
TEST_CASE("test_a_long_hold_without_audio_stops_instead_of_leaving_the_mic_open",
          "[test_personalize_fix_f1_latch_no_data]") {
    Rig rig;
    const double start = monotonic_now();
    rig.runtime->queue_record_press(start);
    rig.runtime->queue_record_release(start + 1.0);
    rig.drain();

    CHECK(rig.control.calls() == Calls{"start", "stop"});
}

// from
// tests/test_personalize_fix_f1_latch_no_data.py::test_a_hold_the_cancel_shortcut_claimed_is_left_alone
TEST_CASE("test_a_hold_the_cancel_shortcut_claimed_is_left_alone",
          "[test_personalize_fix_f1_latch_no_data]") {
    Rig rig;
    const double start = monotonic_now();
    rig.runtime->queue_record_press(start);
    rig.drain();
    rig.control.canceled = true;
    rig.runtime->queue_record_release(start + 1.0);
    rig.drain();

    CHECK(rig.control.calls() == Calls{"start"});
}

// ---- tests/test_push_hold_recording.py::TestPushHoldRuntimeHandlers ----
// The handlers run on the test's thread with the times Python's patched monotonic gave them.

// from tests/test_push_hold_recording.py::TestPushHoldRuntimeHandlers::test_long_hold_stops
TEST_CASE("test_long_hold_stops", "[test_push_hold_recording]") {
    Rig rig;
    as_push_hold_controller(rig);
    rig.runtime->record_key_pressed(100.0);
    rig.runtime->record_key_released(100.5);

    CHECK(rig.control.calls() == Calls{"start", "stop"});
    CHECK(rig.control.sources() == std::vector<std::string>{"hotkey"});
}

// from
// tests/test_push_hold_recording.py::TestPushHoldRuntimeHandlers::test_short_tap_cancels_without_the_hands_free_latch
TEST_CASE("test_short_tap_cancels_without_the_hands_free_latch", "[test_push_hold_recording]") {
    Rig rig;
    as_push_hold_controller(rig);
    rig.set_latch(false);
    rig.runtime->record_key_pressed(100.0);
    rig.runtime->record_key_released(100.1);

    CHECK(rig.control.calls() == Calls{"start", "cancel"});
}

// from
// tests/test_push_hold_recording.py::TestPushHoldRuntimeHandlers::test_refused_start_makes_release_a_no_op
TEST_CASE("test_refused_start_makes_release_a_no_op", "[test_push_hold_recording]") {
    Rig rig;
    as_push_hold_controller(rig);
    rig.control.accept = false;
    rig.runtime->record_key_pressed();
    rig.runtime->record_key_released();

    CHECK(rig.control.calls() == Calls{"start"});
}

// from
// tests/test_push_hold_recording.py::TestPushHoldRuntimeHandlers::test_press_during_post_roll_is_ignored
TEST_CASE("test_press_during_post_roll_is_ignored", "[test_push_hold_recording]") {
    Rig rig;
    as_push_hold_controller(rig);
    rig.control.recording = true;
    rig.runtime->record_key_pressed();

    CHECK(rig.control.calls().empty());
}

TEST_CASE("a press while the recorder closes a capture is ignored", "[dictation][hold]") {
    // The C++ recorder may report a closing capture apart from is_recording; it still owns the
    // recorder, as Python's post-roll does.
    Rig rig;
    as_push_hold_controller(rig);
    rig.control.busy = true;
    rig.runtime->record_key_pressed();

    CHECK(rig.control.calls().empty());
}

// from
// tests/test_push_hold_recording.py::TestPushHoldRuntimeHandlers::test_previous_hold_cannot_cancel_a_profile_recording
TEST_CASE("test_previous_hold_cannot_cancel_a_profile_recording", "[test_push_hold_recording]") {
    Rig rig;
    as_push_hold_controller(rig);
    rig.runtime->record_key_pressed(100.0);
    rig.runtime->record_key_released(100.5);
    CHECK_FALSE(rig.runtime->hold_machine().start_accepted());

    // A profile starts independently. Pressing and releasing the standard hold shortcut must not
    // inherit ownership from the previous hold.
    rig.control.recording = true;
    const auto before = rig.control.calls().size();
    rig.runtime->record_key_pressed();
    rig.runtime->record_key_released();
    CHECK(rig.control.calls().size() == before);
    CHECK(rig.control.recording.load());
}

// from
// tests/test_push_hold_recording.py::TestPushHoldRuntimeHandlers::test_release_before_stream_open_waits_then_no_ops
TEST_CASE("test_release_before_stream_open_waits_then_no_ops", "[test_push_hold_recording]") {
    // A start that never opens the stream: is_recording stays false and the release gives up
    // quietly instead of stopping or canceling.
    Rig rig;
    as_push_hold_controller(rig);
    rig.control.opens_stream = false;

    const auto began = std::chrono::steady_clock::now();
    rig.runtime->record_key_pressed(100.0);
    rig.runtime->record_key_released(100.5);
    const auto waited = std::chrono::steady_clock::now() - began;

    CHECK(rig.control.calls() == Calls{"start"});
    // The wait lasted its half second (fv::hotkeys::kStreamOpenWaitS), not forever.
    CHECK(waited >= std::chrono::milliseconds(450));
    CHECK(waited < std::chrono::seconds(3));
}

TEST_CASE("a release wakes as soon as the stream opens", "[dictation][hold]") {
    // Python polls every 10 ms; here recording_state_changed(true) ends the wait.
    Rig rig;
    as_push_hold_controller(rig);
    rig.control.opens_stream = false;
    rig.runtime->record_key_pressed(100.0);
    std::thread opener([&rig] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        rig.control.recording = true;
        rig.control.stream = true;
        rig.control.recording_state_changed().emit(true);
    });
    const auto began = std::chrono::steady_clock::now();
    rig.runtime->record_key_released(100.5);
    const auto waited = std::chrono::steady_clock::now() - began;
    opener.join();

    CHECK(rig.control.calls() == Calls{"start", "stop"});
    CHECK(waited < std::chrono::milliseconds(400));
}

// from
// tests/test_push_hold_recording.py::TestPushHoldRuntimeHandlers::test_release_after_cancel_hotkey_is_a_no_op
TEST_CASE("test_release_after_cancel_hotkey_is_a_no_op", "[test_push_hold_recording]") {
    // Cancel marks the capture canceled while post-roll keeps is_recording true; the release
    // must not run the stop path.
    Rig rig;
    as_push_hold_controller(rig);
    rig.control.canceled = true;
    rig.runtime->record_key_pressed(100.0);
    rig.runtime->record_key_released(101.0);

    CHECK(rig.control.calls() == Calls{"start"});
}

// from
// tests/test_push_hold_recording.py::TestPushHoldRuntimeHandlers::test_set_mode_applies_to_manager_and_resets_state
TEST_CASE("test_set_mode_applies_to_manager_and_resets_state", "[test_push_hold_recording]") {
    Rig rig;
    as_push_hold_controller(rig);
    // Python sets the press time and the accepted start directly; a press makes both.
    rig.runtime->record_key_pressed(42.0);
    REQUIRE(rig.runtime->hold_machine().start_accepted());
    REQUIRE(rig.runtime->hold_machine().press_time() == 42.0);

    rig.runtime->set_recording_trigger_mode("push_hold");
    rig.drain();
    CHECK(rig.manager->with_matcher([](auto& matcher) { return matcher.record_mode(); }) ==
          TriggerMode::PushHold);
    CHECK_FALSE(rig.runtime->hold_machine().press_time().has_value());
    CHECK_FALSE(rig.runtime->hold_machine().start_accepted());
    CHECK(rig.statuses->values() == std::vector<std::string>{"Push-and-hold recording enabled"});

    rig.runtime->set_recording_trigger_mode("bogus");
    CHECK(rig.manager->with_matcher([](auto& matcher) { return matcher.record_mode(); }) ==
          TriggerMode::Toggle);
    CHECK(rig.statuses->values().back() == "Toggle recording enabled");
}

// ---- tests/test_personalize_s6_runtime.py: the latch and hold tests ----

// from tests/test_personalize_s6_runtime.py::test_a_slow_start_never_lets_its_release_run_first
TEST_CASE("test_a_slow_start_never_lets_its_release_run_first", "[test_personalize_s6_runtime]") {
    Rig rig;
    rig.control.before_start = [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    };
    const double start = monotonic_now();
    rig.runtime->queue_record_press(start);
    rig.runtime->queue_record_release(start + 0.6);
    rig.drain();
    CHECK(rig.control.calls() == Calls{"start", "stop"});
}

// from tests/test_personalize_s6_runtime.py::test_a_second_press_soon_after_a_tap_keeps_recording
TEST_CASE("test_a_second_press_soon_after_a_tap_keeps_recording", "[test_personalize_s6_runtime]") {
    const double gap = GENERATE(0.15, 0.35);
    Rig rig;
    const double pressed_at = rig.tap_then_press(gap);
    rig.runtime->queue_record_release(pressed_at + 0.08); // the latching press's release
    rig.drain();

    CHECK(rig.control.calls() == Calls{"start"});
    CHECK(rig.control.recording.load());
    CHECK(rig.hands_free->values() == std::vector<bool>{true});
    CHECK(rig.statuses->values().back() == "Hands-free · press F9 to stop");
    sleep_s(kWindow + 0.1);
    CHECK(rig.control.calls() == Calls{"start"});

    rig.runtime->queue_record_press(pressed_at + 5);
    rig.runtime->queue_record_release(pressed_at + 5.1);
    rig.drain();
    CHECK(rig.control.calls() == Calls{"start", "stop"});
    CHECK(rig.hands_free->values() == std::vector<bool>{true, false});
}

// from
// tests/test_personalize_s6_runtime.py::test_a_press_after_the_window_cancels_the_tap_and_starts_over
TEST_CASE("test_a_press_after_the_window_cancels_the_tap_and_starts_over",
          "[test_personalize_s6_runtime]") {
    Rig rig;
    rig.tap_then_press(0.45);
    CHECK(rig.control.calls() == Calls{"start", "cancel", "start"});
    CHECK(rig.hands_free->values().empty());
}

TEST_CASE("the restarted hold after a closed window can latch again", "[dictation][hold]") {
    // The recording the window's cancel ended is counted before the new press records its
    // epoch, so the new hold's own tap still waits for a second press.
    Rig rig;
    const double pressed_at = rig.tap_then_press(0.45);
    rig.runtime->queue_record_release(pressed_at + 0.1);
    rig.runtime->queue_record_press(pressed_at + 0.2);
    rig.drain();
    CHECK(rig.control.calls() == Calls{"start", "cancel", "start"});
    CHECK(rig.hands_free->values() == std::vector<bool>{true});
}

// from tests/test_personalize_s6_runtime.py::test_a_lone_tap_cancels_only_once_the_window_closes
TEST_CASE("test_a_lone_tap_cancels_only_once_the_window_closes", "[test_personalize_s6_runtime]") {
    Rig rig;
    const auto began = std::chrono::steady_clock::now();
    const double start = monotonic_now();
    rig.runtime->queue_record_press(start);
    rig.runtime->queue_record_release(start + 0.05);
    rig.drain();
    CHECK(rig.control.calls() == Calls{"start"});
    CHECK(wait_until([&] { return rig.control.calls() == Calls{"start", "cancel"}; }));
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count() >=
          0.05 + kWindow - 0.02);
    CHECK(rig.hands_free->values().empty());
}

// from tests/test_personalize_s6_runtime.py::test_without_the_latch_a_tap_cancels_at_once
TEST_CASE("test_without_the_latch_a_tap_cancels_at_once", "[test_personalize_s6_runtime]") {
    Rig rig;
    rig.set_latch(false);
    rig.tap_then_press(0.15);
    CHECK(rig.control.calls() == Calls{"start", "cancel", "start"});
}

// from tests/test_personalize_s6_runtime.py::test_a_long_hold_still_stops_with_the_latch_on
TEST_CASE("test_a_long_hold_still_stops_with_the_latch_on", "[test_personalize_s6_runtime]") {
    Rig rig;
    const double start = monotonic_now();
    rig.runtime->queue_record_press(start);
    rig.runtime->queue_record_release(start + 0.4);
    rig.drain();
    CHECK(rig.control.calls() == Calls{"start", "stop"});
}

// from tests/test_personalize_s6_runtime.py::test_a_recording_that_ends_elsewhere_leaves_hands_free
TEST_CASE("test_a_recording_that_ends_elsewhere_leaves_hands_free",
          "[test_personalize_s6_runtime]") {
    Rig rig;
    const double pressed_at = rig.tap_then_press(0.15);
    rig.control.stop(); // e.g. the tray
    rig.drain();
    CHECK(rig.hands_free->values() == std::vector<bool>{true, false});

    rig.runtime->queue_record_press(pressed_at + 3);
    rig.drain();
    CHECK(rig.control.calls() == Calls{"start", "stop", "start"});
}

// from
// tests/test_personalize_s6_runtime.py::test_a_tap_canceled_by_the_cancel_key_is_not_canceled_again
TEST_CASE("test_a_tap_canceled_by_the_cancel_key_is_not_canceled_again",
          "[test_personalize_s6_runtime]") {
    Rig rig;
    const double start = monotonic_now();
    rig.runtime->queue_record_press(start);
    rig.runtime->queue_record_release(start + 0.05);
    rig.drain();
    rig.control.cancel();
    sleep_s(kWindow + 0.1);
    rig.drain();
    CHECK(rig.control.calls() == Calls{"start", "cancel"});
}

// from tests/test_personalize_s6_runtime.py::test_changing_the_trigger_mode_drops_the_latch
TEST_CASE("test_changing_the_trigger_mode_drops_the_latch", "[test_personalize_s6_runtime]") {
    Rig rig;
    rig.tap_then_press(0.15);
    rig.runtime->set_recording_trigger_mode("toggle");
    rig.drain();
    CHECK(rig.hands_free->values() == std::vector<bool>{true, false});
    CHECK(rig.control.recording.load());
}

// from tests/test_personalize_s6_runtime.py::test_windows_key_events_latch_and_stop
TEST_CASE("test_windows_key_events_latch_and_stop", "[test_personalize_s6_runtime]") {
    // _load_windows_backend's stub defaults: the record shortcut is "kp *".
    Rig rig({{"record_toggle", "kp *"},
             {"cancel", "kp -"},
             {"enable_disable", "ctrl+alt+kp *"},
             {"minimize_tray", "ctrl+alt+m"},
             {"meeting_toggle", ""}});
    auto& manager = *rig.manager;
    // _wire
    manager.set_record_mode(TriggerMode::PushHold);
    fv::hotkeys::Callbacks callbacks;
    callbacks.on_record_press = [&rig](double at) {
        rig.runtime->queue_record_press(at);
    };
    callbacks.on_record_release = [&rig](double at) {
        rig.runtime->queue_record_release(at);
    };
    manager.set_callbacks(std::move(callbacks));

    for (bool down : {true, false, true, false}) {
        CHECK_FALSE(rig.hook->send(key_event(down, "*")));
    }
    rig.drain();
    CHECK(rig.hands_free->values() == std::vector<bool>{true});
    rig.hook->send(key_event(true, "*"));
    rig.hook->send(key_event(false, "*"));
    rig.drain();
    CHECK(rig.control.calls() == Calls{"start", "stop"});
}
