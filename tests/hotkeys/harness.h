#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "hotkeys/hotkey_manager.h"
#include "hotkeys/testing/fake_keyboard_hook.h"
#include "hotkeys/testing/fake_mouse_hook.h"

// The fixtures of OpenWhisper's Windows backend tests (tests/test_push_hold_recording.py
// _load_windows_backend, tests/test_personalize_s6_backends.py _windows_manager): a manager on a
// fake hook whose keyboard.is_pressed answers from `held`, with the per-press callbacks run inline
// so "never fired" needs no sleep.
namespace fv::hotkeys::testing {

// _load_windows_backend's stub config.DEFAULT_HOTKEYS.
inline std::map<std::string, std::string> stub_default_hotkeys() {
    return {{"record_toggle", "kp *"},
            {"cancel", "kp -"},
            {"enable_disable", "ctrl+alt+kp *"},
            {"minimize_tray", "ctrl+alt+m"},
            {"meeting_toggle", ""}};
}

// test_personalize_s6_backends._windows_hotkeys(**overrides).
inline std::map<std::string, std::string>
windows_hotkeys(const std::map<std::string, std::string>& overrides = {}) {
    auto hotkeys = stub_default_hotkeys();
    hotkeys.insert_or_assign("command_mode", "ctrl+alt+k");
    hotkeys.insert_or_assign("scratchpad_toggle", "ctrl+alt+s");
    hotkeys.insert_or_assign("cycle_language", "ctrl+alt+l");
    hotkeys.insert_or_assign("paste_last_original", "ctrl+alt+o");
    for (const auto& [action, hotkey] : overrides) {
        hotkeys.insert_or_assign(action, hotkey);
    }
    return hotkeys;
}

inline std::uint8_t modifier_bits(const std::set<std::string>& names) {
    std::uint8_t mask = 0;
    mask |= names.contains("ctrl") ? kCtrl : 0;
    mask |= names.contains("alt") ? kAlt : 0;
    mask |= names.contains("shift") ? kShift : 0;
    mask |= names.contains("win") ? kWin : 0;
    return mask;
}

struct Harness {
    explicit Harness(std::map<std::string, std::string> hotkeys) {
        auto fake = std::make_unique<FakeKeyboardHook>();
        hook = fake.get();
        manager = std::make_unique<HotkeyManager>(std::move(hotkeys), std::move(fake));
        manager->set_spawner([](std::function<void()> call) { call(); });
    }

    // A hook event at the next tick of a clock that starts at 100 s; `scan_code` nullopt for the
    // tests' events that carry none.
    KeyEvent event(bool down, std::string_view name, std::optional<int> scan_code,
                   bool keypad = false) {
        KeyEvent e;
        e.down = down;
        e.name = name;
        e.scan_code = scan_code;
        e.keypad = keypad;
        e.t = now;
        now += 0.01;
        e.modifiers = modifier_bits(held);
        return e;
    }
    KeyEvent down(std::string_view name, std::optional<int> scan_code = std::nullopt,
                  bool keypad = false) {
        return event(true, name, scan_code, keypad);
    }
    KeyEvent up(std::string_view name, std::optional<int> scan_code = std::nullopt,
                bool keypad = false) {
        return event(false, name, scan_code, keypad);
    }
    // True when the hook lets the event through to the focused app.
    bool passes(KeyEvent e) {
        e.modifiers = modifier_bits(held);
        return hook->send(e);
    }

    // Gives the manager a fake side-button hook (_windows_manager's _FakeMouseHook), started at
    // once when a shortcut already uses a side button.
    FakeMouseHook* attach_mouse() {
        auto fake = std::make_unique<FakeMouseHook>();
        mouse = fake.get();
        manager->set_mouse_hook_source(std::move(fake));
        return mouse;
    }

    // Records each callback by name, as the Python tests' `events` list does.
    void record_events() {
        Callbacks callbacks;
        callbacks.on_record_toggle = [this] {
            events.push_back("record_toggle");
        };
        callbacks.on_record_press = [this](double) {
            events.push_back("record_press");
        };
        callbacks.on_record_release = [this](double) {
            events.push_back("record_release");
        };
        callbacks.on_command_press = [this](double) {
            events.push_back("command_press");
        };
        callbacks.on_command_release = [this](double) {
            events.push_back("command_release");
        };
        callbacks.on_minimize_tray = [this] {
            events.push_back("minimize");
        };
        callbacks.on_scratchpad_toggle = [this] {
            events.push_back("scratchpad");
        };
        callbacks.on_paste_last_original = [this] {
            events.push_back("paste_original");
        };
        manager->set_callbacks(std::move(callbacks));
    }

    FakeKeyboardHook* hook = nullptr;
    FakeMouseHook* mouse = nullptr;
    std::unique_ptr<HotkeyManager> manager;
    std::set<std::string> held;
    std::vector<std::string> events;
    double now = 100.0;
};

} // namespace fv::hotkeys::testing
