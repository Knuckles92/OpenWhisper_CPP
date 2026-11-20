#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <map>
#include <string>

#include "harness.h"

using namespace fv::hotkeys;
using fv::hotkeys::testing::Harness;
using fv::hotkeys::testing::windows_hotkeys;

namespace {

constexpr int kSc1 = 2;
constexpr int kScCtrl = 29;
constexpr int kScShift = 42;
constexpr int kScAlt = 56;
constexpr int kScM = 50;
constexpr int kScH = 35;
constexpr int kScO = 24;
constexpr int kScT = 20;
constexpr int kScStar = 55;
constexpr int kScSlash = 53;

int modifier_code(const std::string& name) {
    return name == "ctrl" ? kScCtrl : name == "shift" ? kScShift : kScAlt;
}

// tests/test_personalize_fix2_g1_held_scan_code.py::_manager
std::unique_ptr<Harness> manager(TriggerMode mode = TriggerMode::PushHold,
                                 const std::map<std::string, std::string>& hotkeys = {}) {
    auto h = std::make_unique<Harness>(windows_hotkeys(hotkeys));
    h->manager->set_record_mode(mode);
    h->record_events();
    return h;
}

void press_modifiers(Harness& h, std::initializer_list<std::string> names) {
    for (const auto& name : names) {
        h.held.insert(name);
        CHECK(h.passes(h.down(name, modifier_code(name))));
    }
}

void release_modifiers(Harness& h, std::initializer_list<std::string> names) {
    for (const auto& name : names) {
        h.held.erase(name);
        CHECK(h.passes(h.up(name, modifier_code(name))));
    }
}

std::vector<std::string> press_holds(Harness& h) {
    return h.manager->with_matcher([](HotkeyMatcher& m) { return m.press_holds(); });
}

using Events = std::vector<std::string>;

} // namespace

// from
// tests/test_personalize_fix2_g1_held_scan_code.py::test_shift_symbol_shortcut_fires_again_when_shift_is_let_go_first
TEST_CASE("test_shift_symbol_shortcut_fires_again_when_shift_is_let_go_first",
          "[test_personalize_fix2_g1_held_scan_code]") {
    auto h = manager(TriggerMode::PushHold, {{"scratchpad_toggle", "ctrl+shift+!"}});
    for (int i = 0; i < 2; ++i) {
        press_modifiers(*h, {"ctrl", "shift"});
        CHECK_FALSE(h->passes(h->down("!", kSc1)));
        release_modifiers(*h, {"shift", "ctrl"});
        // The key-up of the swallowed press is swallowed too.
        CHECK_FALSE(h->passes(h->up("1", kSc1)));
    }
    CHECK(h->events == Events{"scratchpad", "scratchpad"});
    CHECK(press_holds(*h).empty());
}

// from
// tests/test_personalize_fix2_g1_held_scan_code.py::test_typed_shift_symbol_passes_through_after_the_shortcut
TEST_CASE("test_typed_shift_symbol_passes_through_after_the_shortcut",
          "[test_personalize_fix2_g1_held_scan_code]") {
    auto h = manager(TriggerMode::PushHold, {{"scratchpad_toggle", "ctrl+shift+!"}});
    press_modifiers(*h, {"ctrl", "shift"});
    h->passes(h->down("!", kSc1));
    release_modifiers(*h, {"shift", "ctrl"});
    h->passes(h->up("1", kSc1));

    // Shift+1 typed in another app, released in either order.
    press_modifiers(*h, {"shift"});
    CHECK(h->passes(h->down("!", kSc1)));
    release_modifiers(*h, {"shift"});
    CHECK(h->passes(h->up("1", kSc1)));
    press_modifiers(*h, {"shift"});
    CHECK(h->passes(h->down("!", kSc1)));
    CHECK(h->passes(h->up("!", kSc1)));

    CHECK(h->events == Events{"scratchpad"});
}

// from
// tests/test_personalize_fix2_g1_held_scan_code.py::test_shift_symbol_shortcut_suppresses_its_repeats_until_the_key_comes_up
TEST_CASE("test_shift_symbol_shortcut_suppresses_its_repeats_until_the_key_comes_up",
          "[test_personalize_fix2_g1_held_scan_code]") {
    auto h = manager(TriggerMode::PushHold, {{"scratchpad_toggle", "ctrl+shift+!"}});
    press_modifiers(*h, {"ctrl", "shift"});
    CHECK_FALSE(h->passes(h->down("!", kSc1)));
    CHECK_FALSE(h->passes(h->down("!", kSc1)));
    release_modifiers(*h, {"shift"});
    // Windows keeps repeating the key, now named "1"; nothing reaches the app.
    CHECK_FALSE(h->passes(h->down("1", kSc1)));
    CHECK_FALSE(h->passes(h->up("1", kSc1)));

    CHECK(h->events == Events{"scratchpad"});
}

// from
// tests/test_personalize_fix2_g1_held_scan_code.py::test_command_mode_on_a_shift_symbol_ends_when_shift_is_let_go_first
TEST_CASE("test_command_mode_on_a_shift_symbol_ends_when_shift_is_let_go_first",
          "[test_personalize_fix2_g1_held_scan_code]") {
    auto h = manager(TriggerMode::PushHold,
                     {{"record_toggle", "ctrl+1"}, {"command_mode", "ctrl+shift+!"}});
    press_modifiers(*h, {"ctrl", "shift"});
    CHECK_FALSE(h->passes(h->down("!", kSc1)));
    release_modifiers(*h, {"shift"});
    // The repeat now reads Ctrl+1, the record shortcut, but is the same key.
    CHECK_FALSE(h->passes(h->down("1", kSc1)));
    CHECK_FALSE(h->passes(h->up("1", kSc1)));

    CHECK(h->events == Events{"command_press", "command_release"});
    h->manager->with_matcher([](HotkeyMatcher& m) {
        CHECK_FALSE(m.command_key_held());
        CHECK_FALSE(m.record_key_held());
    });
}

// from
// tests/test_personalize_fix2_g1_held_scan_code.py::test_missed_key_up_never_swallows_later_typing
TEST_CASE("test_missed_key_up_never_swallows_later_typing",
          "[test_personalize_fix2_g1_held_scan_code]") {
    auto h = manager();
    press_modifiers(*h, {"ctrl", "alt"});
    CHECK_FALSE(h->passes(h->down("m", kScM)));
    // The "m" key-up went to an elevated window; the hook never saw it.
    release_modifiers(*h, {"alt", "ctrl"});

    CHECK(h->passes(h->down("h", kScH)));
    CHECK(h->passes(h->up("h", kScH)));
    CHECK(h->passes(h->down("m", kScM)));
    CHECK(h->passes(h->up("m", kScM)));

    press_modifiers(*h, {"ctrl", "alt"});
    CHECK_FALSE(h->passes(h->down("m", kScM)));
    CHECK(h->events == Events{"minimize", "minimize"});
}

// from
// tests/test_personalize_fix2_g1_held_scan_code.py::test_key_up_missed_behind_an_elevated_window_frees_the_key_later
TEST_CASE("test_key_up_missed_behind_an_elevated_window_frees_the_key_later",
          "[test_personalize_fix2_g1_held_scan_code]") {
    Harness h(windows_hotkeys());
    std::vector<std::string> fired;
    Callbacks callbacks;
    callbacks.on_minimize_tray = [&fired] {
        fired.push_back("minimize");
    };
    h.manager->set_callbacks(callbacks);
    h.now = 100.0;

    // The clock stands still between these events, as the test's fake clock does.
    KeyEvent ctrl = h.down("ctrl", kScCtrl);
    KeyEvent alt = h.down("alt", kScAlt);
    KeyEvent m = h.down("m", kScM);
    ctrl.t = alt.t = m.t = 100.0;
    h.held.insert("ctrl");
    CHECK(h.passes(ctrl));
    h.held.insert("alt");
    CHECK(h.passes(alt));
    CHECK_FALSE(h.passes(m));
    // The elevated window got every key-up and the typing after it; the hook saw none of it.
    // Back in a normal app the user types "m" first.
    h.held.clear();
    m.t = 105.0;
    CHECK(h.passes(m));
    KeyEvent m_up = h.up("m", kScM);
    m_up.t = 105.0;
    CHECK(h.passes(m_up));

    CHECK(fired == std::vector<std::string>{"minimize"});
}

// from
// tests/test_personalize_fix2_g1_held_scan_code.py::test_slow_auto_repeat_still_counts_as_a_repeat
TEST_CASE("test_slow_auto_repeat_still_counts_as_a_repeat",
          "[test_personalize_fix2_g1_held_scan_code]") {
    Harness h(windows_hotkeys());
    std::vector<std::string> fired;
    Callbacks callbacks;
    callbacks.on_paste_last_original = [&fired] {
        fired.push_back("paste_original");
    };
    h.manager->set_callbacks(callbacks);

    KeyEvent o = h.down("o", kScO);
    h.held = {"ctrl", "alt"};
    o.t = 100.0;
    CHECK_FALSE(h.passes(o));
    // Filter Keys allows a 2 s repeat delay and rate.
    for (int i = 0; i < 3; ++i) {
        o.t += 2.0;
        CHECK_FALSE(h.passes(o));
    }
    CHECK(fired == std::vector<std::string>{"paste_original"});
}

// from
// tests/test_personalize_fix2_g1_held_scan_code.py::test_missed_key_up_never_strands_a_transform_shortcut
TEST_CASE("test_missed_key_up_never_strands_a_transform_shortcut",
          "[test_personalize_fix2_g1_held_scan_code]") {
    auto h = manager();
    std::vector<std::string> fired;
    h->manager->set_dynamic_hotkeys("transform", {{"polish", "ctrl+alt+t"}},
                                    [&fired](const std::string& id) { fired.push_back(id); });
    press_modifiers(*h, {"ctrl", "alt"});
    CHECK_FALSE(h->passes(h->down("t", kScT)));
    release_modifiers(*h, {"alt", "ctrl"});
    CHECK(h->passes(h->down("h", kScH)));
    h->manager->with_matcher(
        [](HotkeyMatcher& m) { m.dynamic_debouncer("transform", "polish")->reset(); });

    press_modifiers(*h, {"ctrl", "alt"});
    CHECK_FALSE(h->passes(h->down("t", kScT)));

    CHECK(fired == std::vector<std::string>{"polish", "polish"});
}

// from
// tests/test_personalize_fix2_g1_held_scan_code.py::test_numpad_star_repeat_after_ctrl_is_let_go_stays_with_command_mode
TEST_CASE("test_numpad_star_repeat_after_ctrl_is_let_go_stays_with_command_mode",
          "[test_personalize_fix2_g1_held_scan_code]") {
    auto h =
        manager(TriggerMode::PushHold, {{"record_toggle", "kp *"}, {"command_mode", "ctrl+kp *"}});
    for (int i = 0; i < 2; ++i) {
        press_modifiers(*h, {"ctrl"});
        CHECK_FALSE(h->passes(h->down("*", kScStar, true)));
        CHECK_FALSE(h->passes(h->down("*", kScStar, true)));
        release_modifiers(*h, {"ctrl"});
        CHECK_FALSE(h->passes(h->down("*", kScStar, true)));
        CHECK_FALSE(h->passes(h->up("*", kScStar, true)));
    }
    CHECK(h->events ==
          Events{"command_press", "command_release", "command_press", "command_release"});
    h->manager->with_matcher([](HotkeyMatcher& m) { CHECK_FALSE(m.record_key_held()); });
}

// from
// tests/test_personalize_fix2_g1_held_scan_code.py::test_paste_original_fires_once_per_physical_press
TEST_CASE("test_paste_original_fires_once_per_physical_press",
          "[test_personalize_fix2_g1_held_scan_code]") {
    auto h = manager();
    press_modifiers(*h, {"ctrl", "alt"});
    for (int i = 0; i < 5; ++i) {
        CHECK_FALSE(h->passes(h->down("o", kScO)));
    }
    CHECK_FALSE(h->passes(h->up("o", kScO)));
    // Second physical press with the modifiers still held.
    CHECK_FALSE(h->passes(h->down("o", kScO)));
    CHECK(h->events == Events{"paste_original", "paste_original"});
}

// from
// tests/test_personalize_fix2_g1_held_scan_code.py::test_main_key_with_the_same_scan_code_does_not_end_a_numpad_hold
TEST_CASE("test_main_key_with_the_same_scan_code_does_not_end_a_numpad_hold",
          "[test_personalize_fix2_g1_held_scan_code]") {
    auto h = manager(TriggerMode::PushHold, {{"record_toggle", "kp /"}});
    CHECK_FALSE(h->passes(h->down("/", kScSlash, true)));
    // The main-row "/" shares the numpad key's scan code.
    CHECK(h->passes(h->up("/", kScSlash, false)));
    h->manager->with_matcher([](HotkeyMatcher& m) { CHECK(m.record_key_held()); });
    CHECK_FALSE(h->passes(h->up("/", kScSlash, true)));
    CHECK(h->events == Events{"record_press", "record_release"});
}

// from
// tests/test_personalize_fix_f1_hotkey_repeat.py::test_command_hold_survives_a_repeat_after_its_modifier_is_released
TEST_CASE("test_command_hold_survives_a_repeat_after_its_modifier_is_released",
          "[test_personalize_fix_f1_hotkey_repeat]") {
    struct Row {
        std::string record, command, key;
        bool keypad;
        std::set<std::string> chord, let_go_first;
    };
    const Row row = GENERATE(
        Row{"kp *", "ctrl+kp *", "*", true, {"ctrl"}, {"ctrl"}},
        Row{"ctrl+space", "ctrl+shift+space", "space", false, {"ctrl", "shift"}, {"shift"}});
    const TriggerMode mode = GENERATE(TriggerMode::PushHold, TriggerMode::Toggle);
    auto h = manager(mode, {{"record_toggle", row.record},
                            {"command_mode", row.command},
                            {"enable_disable", "ctrl+alt+shift+f12"}});
    // The tests' events carry no scan code: the key is known by its name.
    const KeyEvent down = h->down(row.key, std::nullopt, row.keypad);
    const KeyEvent up = h->up(row.key, std::nullopt, row.keypad);

    h->held.insert(row.chord.begin(), row.chord.end());
    CHECK_FALSE(h->passes(down));
    CHECK_FALSE(h->passes(down));
    for (const auto& name : row.let_go_first) {
        h->held.erase(name);
    }
    // Still suppressed: the repeat belongs to the held Command Mode key.
    CHECK_FALSE(h->passes(down));
    CHECK_FALSE(h->passes(up));
    h->held.clear();

    CHECK(h->events == Events{"command_press", "command_release"});
    h->manager->with_matcher([](HotkeyMatcher& m) {
        CHECK_FALSE(m.command_key_held());
        CHECK_FALSE(m.record_key_held());
    });

    h->held.insert(row.chord.begin(), row.chord.end());
    h->passes(down);
    CHECK(h->events.back() == "command_press");
}

// from
// tests/test_personalize_fix_f1_hotkey_repeat.py::test_release_ends_every_hold_on_that_main_key
TEST_CASE("test_release_ends_every_hold_on_that_main_key",
          "[test_personalize_fix_f1_hotkey_repeat]") {
    auto h = manager(TriggerMode::PushHold, {{"record_toggle", "kp *"},
                                             {"command_mode", "ctrl+kp *"},
                                             {"enable_disable", "ctrl+alt+shift+f12"}});
    h->held = {"ctrl"};
    // Both holds set at once, as a missed KEY_UP or rehook could leave them.
    h->manager->with_matcher([](HotkeyMatcher& m) {
        m.set_record_key_held(true);
        m.set_command_key_held(true);
    });

    CHECK_FALSE(h->passes(h->up("*", std::nullopt, true)));

    CHECK(h->events == Events{"record_release", "command_release"});
    h->manager->with_matcher([](HotkeyMatcher& m) {
        CHECK_FALSE(m.record_key_held());
        CHECK_FALSE(m.command_key_held());
    });
}

// from
// tests/test_personalize_fix_f1_hotkey_repeat.py::test_paste_original_fires_once_per_press_despite_auto_repeat
TEST_CASE("test_paste_original_fires_once_per_press_despite_auto_repeat",
          "[test_personalize_fix_f1_hotkey_repeat]") {
    auto h = manager(TriggerMode::PushHold);
    h->held = {"ctrl", "alt"};
    const KeyEvent down = h->down("o", std::nullopt, false);
    for (int i = 0; i < 5; ++i) {
        CHECK_FALSE(h->passes(down));
    }
    CHECK(h->events == Events{"paste_original"});

    h->held.clear();
    CHECK_FALSE(h->passes(h->up("o", std::nullopt, false)));
    h->held = {"ctrl", "alt"};
    h->passes(down);
    CHECK(h->events == Events{"paste_original", "paste_original"});
}

// from
// tests/test_personalize_fix_f1_hotkey_repeat.py::test_forgetting_held_keys_frees_a_press_action
TEST_CASE("test_forgetting_held_keys_frees_a_press_action",
          "[test_personalize_fix_f1_hotkey_repeat]") {
    auto h = manager(TriggerMode::PushHold);
    h->held = {"ctrl", "alt"};
    const KeyEvent down = h->down("o", std::nullopt, false);
    h->passes(down);

    h->manager->set_capture_suspended(true);
    h->manager->set_capture_suspended(false);
    h->passes(down);

    CHECK(h->events == Events{"paste_original", "paste_original"});
}
