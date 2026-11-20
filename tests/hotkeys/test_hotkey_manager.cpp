#include <catch2/catch_test_macros.hpp>

#include "harness.h"

using fv::hotkeys::HotkeyMatcher;
using fv::hotkeys::testing::Harness;
using fv::hotkeys::testing::stub_default_hotkeys;

// from tests/test_hotkey_manager.py::TestHotkeyManager::test_record_toggle_uses_monotonic_time
TEST_CASE("test_record_toggle_uses_monotonic_time", "[test_hotkey_manager]") {
    Harness h(stub_default_hotkeys());
    h.manager->with_matcher([](HotkeyMatcher& m) {
        CHECK(m.should_trigger_record_toggle(100.0));
        CHECK_FALSE(m.should_trigger_record_toggle(100.05));
        CHECK(m.should_trigger_record_toggle(100.4));
    });
}

// from tests/test_hotkey_manager.py::TestHotkeyManager::test_enable_toggle_clears_debounce_state
TEST_CASE("test_enable_toggle_clears_debounce_state", "[test_hotkey_manager]") {
    Harness h(stub_default_hotkeys());
    h.manager->with_matcher([](HotkeyMatcher& m) { CHECK(m.should_trigger_record_toggle(100.0)); });

    h.manager->toggle_program_enabled();
    h.manager->with_matcher([](HotkeyMatcher& m) {
        CHECK_FALSE(m.record_debouncer().last_trigger_time().has_value());
        CHECK(m.should_trigger_record_toggle(100.01));
    });
}

// from tests/test_hotkey_manager.py::TestHotkeyManager::test_rehook_preserves_state
TEST_CASE("test_rehook_preserves_state", "[test_hotkey_manager]") {
    Harness h(stub_default_hotkeys());
    h.manager->set_program_enabled(false);
    const auto original_hotkeys = h.manager->hotkeys();
    int toggles = 0;
    fv::hotkeys::Callbacks callbacks;
    callbacks.on_record_toggle = [&toggles] {
        ++toggles;
    };
    h.manager->set_callbacks(callbacks);

    h.manager->rehook();

    CHECK(h.hook->uninstalls() == 1);
    CHECK(h.hook->installs() == 2);
    CHECK_FALSE(h.manager->program_enabled());
    CHECK(h.manager->hotkeys() == original_hotkeys);
    // The callback is still the one set: re-enable and press the record key.
    h.manager->set_program_enabled(true);
    CHECK_FALSE(h.passes(h.down("*", 55, true)));
    CHECK(toggles == 1);
}
