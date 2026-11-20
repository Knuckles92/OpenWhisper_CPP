#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <stdexcept>
#include <thread>

#include "harness.h"
#include "hotkeys/grammar.h"

using namespace fv::hotkeys;
using fv::hotkeys::testing::Harness;
using fv::hotkeys::testing::stub_default_hotkeys;
using fv::hotkeys::testing::windows_hotkeys;

// from
// tests/test_personalize_s6_backends.py::test_side_buttons_parse_as_main_keys_and_display_by_number
TEST_CASE("test_side_buttons_parse_as_main_keys_and_display_by_number",
          "[test_personalize_s6_backends]") {
    CHECK(parse_hotkey("ctrl+mouse4", Vocabulary::Windows) ==
          ParsedHotkey{{"ctrl"}, std::string("mouse4")});
    CHECK(format_hotkey_display("mouse5", Vocabulary::Windows) == "Mouse 5");
    CHECK(format_hotkey_display("ctrl+shift+mouse4", Vocabulary::Windows) == "Ctrl+Shift+Mouse 4");

    CHECK(parse_hotkey("alt+mouse5", Vocabulary::Pynput) ==
          ParsedHotkey{{"alt"}, std::string("mouse5")});
    CHECK(format_hotkey_display("ctrl+mouse4", Vocabulary::Pynput, false) == "Ctrl+Mouse 4");
    CHECK(is_mouse_key("mouse4"));
    CHECK_FALSE(is_mouse_key("m"));
}

// from
// tests/test_personalize_s6_backends.py::test_windows_key_spelled_by_keyboard_library_matches_as_win
TEST_CASE("test_windows_key_spelled_by_keyboard_library_matches_as_win",
          "[test_personalize_s6_backends]") {
    Harness h(windows_hotkeys({{"record_toggle", "windows+f9"}}));
    h.held = {"win"};
    CHECK(parse_hotkey("windows+f9", Vocabulary::Windows) ==
          ParsedHotkey{{"win"}, std::string("f9")});
    CHECK(format_hotkey_display("windows+f9", Vocabulary::Windows) == "Win+F9");
    int toggles = 0;
    Callbacks callbacks;
    callbacks.on_record_toggle = [&toggles] {
        ++toggles;
    };
    h.manager->set_callbacks(callbacks);

    CHECK_FALSE(h.passes(h.down("f9", std::nullopt, false)));
    CHECK(toggles == 1);
}

// from tests/test_personalize_s6_backends.py::test_windows_press_actions_fire_their_own_callbacks
TEST_CASE("test_windows_press_actions_fire_their_own_callbacks", "[test_personalize_s6_backends]") {
    Harness h(windows_hotkeys());
    h.held = {"ctrl", "alt"};
    std::vector<std::string> fired;
    Callbacks callbacks;
    callbacks.on_scratchpad_toggle = [&] {
        fired.push_back("scratchpad");
    };
    callbacks.on_cycle_language = [&] {
        fired.push_back("language");
    };
    callbacks.on_paste_last_original = [&] {
        fired.push_back("original");
    };
    h.manager->set_callbacks(callbacks);

    for (const auto& [key, name] :
         {std::pair{"s", "scratchpad"}, std::pair{"l", "language"}, std::pair{"o", "original"}}) {
        CHECK_FALSE(h.passes(h.down(key, std::nullopt, false)));
        REQUIRE_FALSE(fired.empty());
        CHECK(fired.back() == name);
    }
    CHECK(h.passes(h.down("q", std::nullopt, false)));
    CHECK(fired.size() == 3);
}

// from
// tests/test_personalize_s6_backends.py::test_windows_stub_hotkeys_without_new_actions_still_dispatch
TEST_CASE("test_windows_stub_hotkeys_without_new_actions_still_dispatch",
          "[test_personalize_s6_backends]") {
    Harness h(stub_default_hotkeys());
    CHECK_FALSE(h.manager->hotkeys().contains("command_mode"));
    int cancels = 0;
    Callbacks callbacks;
    callbacks.on_cancel = [&cancels] {
        ++cancels;
    };
    h.manager->set_callbacks(callbacks);
    CHECK_FALSE(h.passes(h.down("-", std::nullopt, true)));
    CHECK(cancels == 1);
    CHECK(h.passes(h.down("x", std::nullopt, false)));
}

// from
// tests/test_personalize_s6_backends.py::test_windows_command_mode_sends_stamped_press_and_release_once
TEST_CASE("test_windows_command_mode_sends_stamped_press_and_release_once",
          "[test_personalize_s6_backends]") {
    Harness h(windows_hotkeys());
    h.held = {"ctrl", "alt"};
    std::vector<double> presses;
    std::vector<double> releases;
    Callbacks callbacks;
    callbacks.on_command_press = [&](double at) {
        presses.push_back(at);
    };
    callbacks.on_command_release = [&](double at) {
        releases.push_back(at);
    };
    h.manager->set_callbacks(callbacks);
    KeyEvent down = h.down("k", std::nullopt, false);
    KeyEvent up = h.up("k", std::nullopt, false);

    down.t = 5.0;
    CHECK_FALSE(h.passes(down));
    CHECK_FALSE(h.passes(down)); // auto-repeat
    h.held.clear();              // modifiers let go first
    up.t = 6.0;
    CHECK_FALSE(h.passes(up));

    CHECK(presses == std::vector<double>{5.0});
    CHECK(releases == std::vector<double>{6.0});
    CHECK(h.passes(up));
}

// from
// tests/test_personalize_s6_backends.py::test_windows_command_release_survives_disabling_hotkeys_mid_hold
TEST_CASE("test_windows_command_release_survives_disabling_hotkeys_mid_hold",
          "[test_personalize_s6_backends]") {
    Harness h(windows_hotkeys());
    h.held = {"ctrl", "alt"};
    int releases = 0;
    Callbacks callbacks;
    callbacks.on_command_press = [](double) {
    };
    callbacks.on_command_release = [&releases](double) {
        ++releases;
    };
    h.manager->set_callbacks(callbacks);
    h.passes(h.down("k", std::nullopt, false));
    h.manager->set_program_enabled(false);
    CHECK_FALSE(h.passes(h.up("k", std::nullopt, false)));
    CHECK(releases == 1);
}

// from
// tests/test_personalize_s6_backends.py::test_windows_push_hold_press_runs_inside_the_hook_with_its_time
TEST_CASE("test_windows_push_hold_press_runs_inside_the_hook_with_its_time",
          "[test_personalize_s6_backends]") {
    Harness h(windows_hotkeys());
    h.manager->set_record_mode(TriggerMode::PushHold);
    std::vector<std::tuple<std::string, double, std::thread::id>> seen;
    Callbacks callbacks;
    callbacks.on_record_press = [&](double at) {
        seen.emplace_back("press", at, std::this_thread::get_id());
    };
    callbacks.on_record_release = [&](double at) {
        seen.emplace_back("release", at, std::this_thread::get_id());
    };
    h.manager->set_callbacks(callbacks);
    // Spawned callbacks would run elsewhere; these two must not.
    h.manager->set_spawner([](std::function<void()> call) { std::thread(call).join(); });

    KeyEvent down = h.down("*", std::nullopt, true);
    KeyEvent up = h.up("*", std::nullopt, true);
    down.t = 1.0;
    up.t = 1.1;
    h.passes(down);
    h.passes(up);
    const auto me = std::this_thread::get_id();
    CHECK(seen == std::vector<std::tuple<std::string, double, std::thread::id>>{
                      {"press", 1.0, me}, {"release", 1.1, me}});
}

// from
// tests/test_personalize_s6_backends.py::test_windows_transform_family_dispatches_beside_profiles
TEST_CASE("test_windows_transform_family_dispatches_beside_profiles",
          "[test_personalize_s6_backends]") {
    Harness h(windows_hotkeys());
    h.held = {"ctrl", "alt"};
    std::vector<std::string> profiles;
    std::vector<std::string> transforms;
    const auto profile_callback = [&profiles](const std::string& id) {
        profiles.push_back(id);
    };
    const auto transform_callback = [&transforms](const std::string& id) {
        transforms.push_back(id);
    };
    h.manager->set_dynamic_hotkeys("profile", {{"ticket", "ctrl+alt+t"}}, profile_callback);
    h.manager->set_dynamic_hotkeys("transform", {{"polish", "ctrl+alt+p"}}, transform_callback);
    h.manager->set_dynamic_hotkeys("profile", {{"ticket", "ctrl+alt+t"}}, profile_callback);

    CHECK(h.manager->dynamic_hotkeys().at("transform:polish") == "ctrl+alt+p");
    const KeyEvent down = h.down("p", std::nullopt, false);
    CHECK_FALSE(h.passes(down));
    CHECK(transforms == std::vector<std::string>{"polish"});
    h.passes(down);
    CHECK_FALSE(h.passes(h.up("p", std::nullopt, false)));
    CHECK(transforms == std::vector<std::string>{"polish"});

    h.manager->set_dynamic_hotkeys("transform", {}, transform_callback);
    CHECK(h.passes(h.down("p", std::nullopt, false)));
    CHECK_THROWS_AS(h.manager->set_dynamic_hotkeys("macro", {}, transform_callback),
                    std::invalid_argument);
    // ValueError(f"Unknown shortcut family: {namespace}").
    CHECK_THROWS_WITH(h.manager->set_dynamic_hotkeys("macro", {}, transform_callback),
                      "Unknown shortcut family: macro");
    CHECK(profiles.empty());
}
