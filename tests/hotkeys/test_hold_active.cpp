#include <catch2/catch_test_macros.hpp>

#include "harness.h"

using namespace fv::hotkeys;
using fv::hotkeys::testing::Harness;
using fv::hotkeys::testing::windows_hotkeys;

// from tests/test_hook_refresh_waits_for_hold.py::test_a_hold_without_recent_repeats_is_stale
TEST_CASE("test_a_hold_without_recent_repeats_is_stale", "[test_hook_refresh_waits_for_hold]") {
    Harness h(windows_hotkeys());
    h.manager->set_record_mode(TriggerMode::PushHold);
    const KeyEvent down = h.down("*", std::nullopt, true);
    h.passes(down);
    const double pressed_at = down.t;

    CHECK(h.manager->keyboard_hold_active(pressed_at + 1.0));
    CHECK_FALSE(h.manager->keyboard_hold_active(pressed_at + 3.0));
}

// from tests/test_hook_refresh_waits_for_hold.py::test_another_key_after_the_press_ends_the_wait
TEST_CASE("test_another_key_after_the_press_ends_the_wait", "[test_hook_refresh_waits_for_hold]") {
    // The runtime wires push-and-hold (_wired_windows_manager); Windows repeats only the last
    // key pressed, so the hold can no longer be proven.
    Harness h(windows_hotkeys());
    h.manager->set_record_mode(TriggerMode::PushHold);
    h.manager->set_callbacks({.on_record_press = [](double) {},
                              .on_record_release =
                                  [](double) {
                                  }});
    h.passes(h.down("*", std::nullopt, true));
    const KeyEvent other = h.down("a", std::nullopt, false);
    h.passes(other);

    CHECK_FALSE(h.manager->keyboard_hold_active(other.t));
}
