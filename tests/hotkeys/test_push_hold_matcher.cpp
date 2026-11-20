#include <catch2/catch_test_macros.hpp>

#include "harness.h"

using namespace fv::hotkeys;
using fv::hotkeys::testing::Harness;
using fv::hotkeys::testing::stub_default_hotkeys;

namespace {

struct Log {
    int presses = 0;
    int releases = 0;
    int toggles = 0;
};

void log_callbacks(Harness& h, Log& log) {
    Callbacks callbacks;
    callbacks.on_record_toggle = [&log] {
        ++log.toggles;
    };
    callbacks.on_record_press = [&log](double) {
        ++log.presses;
    };
    callbacks.on_record_release = [&log](double) {
        ++log.releases;
    };
    h.manager->set_callbacks(std::move(callbacks));
}

} // namespace

// from
// tests/test_push_hold_recording.py::TestWindowsBackendPushHold::test_hold_fires_press_once_and_release_on_key_up
TEST_CASE("test_hold_fires_press_once_and_release_on_key_up", "[test_push_hold_recording]") {
    Harness h(stub_default_hotkeys());
    h.manager->set_record_mode(TriggerMode::PushHold);
    Log log;
    log_callbacks(h, log);

    CHECK_FALSE(h.passes(h.down("*", std::nullopt, true)));
    CHECK(log.presses == 1);
    // Auto-repeat key-downs while held must not fire the press again.
    h.passes(h.down("*", std::nullopt, true));
    CHECK_FALSE(h.passes(h.up("*", std::nullopt, true)));

    CHECK(log.presses == 1);
    CHECK(log.releases == 1);
    h.manager->with_matcher([](HotkeyMatcher& m) { CHECK_FALSE(m.record_key_held()); });
}

// from
// tests/test_push_hold_recording.py::TestWindowsBackendPushHold::test_release_matches_main_key_even_without_modifiers
TEST_CASE("test_release_matches_main_key_even_without_modifiers", "[test_push_hold_recording]") {
    Harness h(stub_default_hotkeys());
    h.manager->update_hotkeys({{"record_toggle", "ctrl+alt+r"}});
    h.manager->set_record_mode(TriggerMode::PushHold);
    Log log;
    log_callbacks(h, log);

    h.held = {"ctrl", "alt"};
    CHECK_FALSE(h.passes(h.down("r", std::nullopt, false)));
    CHECK(log.presses == 1);

    // Modifiers released before the main key; the release must still match.
    h.held.clear();
    CHECK_FALSE(h.passes(h.up("r", std::nullopt, false)));
    CHECK(log.releases == 1);
}

// from
// tests/test_push_hold_recording.py::TestWindowsBackendPushHold::test_toggle_mode_suppresses_release_without_dispatching
TEST_CASE("test_toggle_mode_suppresses_release_without_dispatching", "[test_push_hold_recording]") {
    Harness h(stub_default_hotkeys());
    Log log;
    log_callbacks(h, log);

    CHECK_FALSE(h.passes(h.down("*", std::nullopt, true)));
    CHECK(log.toggles == 1);
    CHECK_FALSE(h.passes(h.up("*", std::nullopt, true)));
    CHECK(log.releases == 0);
}

// from
// tests/test_push_hold_recording.py::TestWindowsBackendPushHold::test_release_without_press_passes_through
TEST_CASE("test_release_without_press_passes_through", "[test_push_hold_recording]") {
    Harness h(stub_default_hotkeys());
    Log log;
    log_callbacks(h, log);

    CHECK(h.passes(h.up("*", std::nullopt, true)));
    CHECK(log.releases == 0);
}

// from
// tests/test_push_hold_recording.py::TestPynputBackendPushHold::test_toggle_mode_press_still_fires_toggle
// The pynput backend is other-platform; its toggle-mode contract (a press of the record combo
// fires on_record_toggle) is what the Windows matcher must keep too.
TEST_CASE("test_toggle_mode_press_still_fires_toggle", "[test_push_hold_recording]") {
    Harness h(stub_default_hotkeys());
    h.manager->update_hotkeys({{"record_toggle", "ctrl+alt+r"}});
    Log log;
    log_callbacks(h, log);

    h.held = {"ctrl", "alt"};
    CHECK_FALSE(h.passes(h.down("r", std::nullopt, false)));
    CHECK(log.toggles == 1);
}
