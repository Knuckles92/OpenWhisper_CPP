#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "hotkeys/hotkey_manager.h"
#include "hotkeys/key_event.h"
#include "hotkeys/key_names.h"
#include "hotkeys/testing/fake_keyboard_hook.h"
#include "testkit/testkit.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace fv::hotkeys;

namespace {

RawKey raw(int vk, int scan_code, bool down = true, bool extended = false) {
    RawKey key;
    key.vk = vk;
    key.scan_code = scan_code;
    key.down = down;
    key.extended = extended;
    return key;
}

constexpr int kVkLControl = 0xA2;
constexpr int kVkLMenu = 0xA4;
constexpr int kVkLShift = 0xA0;
constexpr int kVkMultiply = 0x6A;

} // namespace

TEST_CASE("the generated names are keyboard's for the keys saved shortcuts use", "[hotkeys]") {
    const auto& names = KeyNameTable::generated();
    CHECK(keyboard_data().layout == "00000409");
    CHECK(names.name(55, kVkMultiply, false, 0) == "*"); // numpad *, saved as "kp *"
    CHECK(names.name(74, 0x6D, false, 0) == "-");        // numpad -, "kp -"
    CHECK(names.name(9, 0x38, false, 0) == "8");
    CHECK(names.name(9, 0x38, false, kNameShift) == "*"); // Shift+8 is the main-row "*"
    CHECK(names.name(2, 0x31, false, kNameShift) == "!"); // Shift+1 goes down as "!"
    CHECK(names.name(73, 0x21, true, 0) == "page up");
    CHECK(names.name(1, 0x1B, false, 0) == "esc");
    CHECK(names.name(57, 0x20, false, 0) == "space");
    CHECK(names.name(50, 0x4D, false, kNameShift) == "M"); // single characters keep case
    CHECK(names.name(29, kVkLControl, false, 0) == "ctrl");
    CHECK(names.name(29, 0xA3, true, 0) == "right ctrl");
    CHECK(names.name(541, kVkLControl, false, 0) == "alt gr");
    CHECK(names.name(541, kVkLControl, false, kNameScrollLock) == "ctrl");
    CHECK(names.name(0, kVkMultiply, false, 0) == "*"); // virtual-key input
    CHECK(names.name(9999, 1, false, 0).empty());

    CHECK(is_keypad_key(55, kVkMultiply, false));
    CHECK(is_keypad_key(28, 0x0D, true));
    CHECK_FALSE(is_keypad_key(28, 0x0D, false));
    CHECK(keyboard_data().modifier_scan_codes[0] == std::vector<int>{29, 57629, 57373});
}

TEST_CASE("key names normalize as keyboard's normalize_name does", "[hotkeys]") {
    CHECK(normalize_key_name("Esc") == "esc");
    CHECK(normalize_key_name("Escape") == "esc");
    CHECK(normalize_key_name("\r") == "enter");
    CHECK(normalize_key_name(" ") == "space");
    CHECK(normalize_key_name("Page_Up") == "page up");
    CHECK(normalize_key_name("_") == "_");
    CHECK(normalize_key_name("A") == "A");
    CHECK(normalize_key_name("Num 7") == "7");
    CHECK(normalize_key_name("Right Alt") == "right alt");
    CHECK_THROWS(normalize_key_name(""));
}

TEST_CASE("the translator names events by Shift and lock state and tracks pressed modifiers",
          "[hotkeys]") {
    KeyEventTranslator translator(KeyNameTable::generated());
    const LockState no_locks;

    auto shift = translator.translate(raw(kVkLShift, 42), no_locks, 1.0, std::nullopt);
    REQUIRE(shift);
    CHECK(shift->name == "shift");
    CHECK(shift->modifiers == 0); // keyboard.is_pressed reads the state before this event
    translator.passed(*shift);

    auto eight = translator.translate(raw(0x38, 9), no_locks, 1.1, std::nullopt);
    REQUIRE(eight);
    CHECK(eight->name == "*");
    CHECK_FALSE(eight->keypad);
    CHECK(eight->modifiers == kShift);
    CHECK(eight->scan_code == 9);

    auto star = translator.translate(raw(kVkMultiply, 55), no_locks, 1.2, std::nullopt);
    REQUIRE(star);
    CHECK(star->name == "*");
    CHECK(star->keypad);

    // A swallowed key is never recorded as pressed.
    auto ctrl = translator.translate(raw(kVkLControl, 29), no_locks, 1.3, std::nullopt);
    REQUIRE(ctrl);
    CHECK(translator.pressed_modifiers() == kShift);
    translator.passed(*ctrl);
    CHECK(translator.pressed_modifiers() == (kShift | kCtrl));

    auto shift_up = translator.translate(raw(kVkLShift, 42, false), no_locks, 1.4, std::nullopt);
    REQUIRE(shift_up);
    translator.passed(*shift_up);
    auto one = translator.translate(raw(0x31, 2), no_locks, 1.5, std::nullopt);
    REQUIRE(one);
    CHECK(one->name == "1");
    CHECK(one->modifiers == kCtrl);

    // Numpad 8 with Num Lock on reports VK_NUMPAD8, with it off VK_UP: both are numpad keys.
    auto pad8 = translator.translate(raw(0x68, 72), {.num_lock = true}, 1.6, std::nullopt);
    REQUIRE(pad8);
    CHECK(pad8->name == "8");
    CHECK(pad8->keypad);
    auto pad_up = translator.translate(raw(0x26, 72), no_locks, 1.7, std::nullopt);
    REQUIRE(pad_up);
    CHECK(pad_up->name == "up");
    CHECK(pad_up->keypad);
}

TEST_CASE("the translator passes on what keyboard never reports", "[hotkeys]") {
    KeyEventTranslator translator(KeyNameTable::generated());
    const LockState no_locks;
    CHECK_FALSE(translator.translate(raw(0xE7, 0), no_locks, 1.0, std::nullopt)); // VK_PACKET
    RawKey injected_alt = raw(0x4D, 50);
    injected_alt.injected_alt = true;
    CHECK_FALSE(translator.translate(injected_alt, no_locks, 1.0, std::nullopt));

    // AltGr: a fake left Ctrl with scan code 541, then a right Alt keyboard drops.
    auto fake_ctrl = translator.translate(raw(kVkLControl, 541), no_locks, 1.0, std::nullopt);
    REQUIRE(fake_ctrl);
    CHECK(fake_ctrl->name == "alt gr");
    CHECK_FALSE(translator.translate(raw(0xA5, 56, true, true), no_locks, 1.0, std::nullopt));
    auto right_alt = translator.translate(raw(0xA5, 56, false, true), no_locks, 1.1, std::nullopt);
    CHECK(right_alt);

    // Virtual-key input carries -vk and the OS's modifiers.
    auto virtual_key = translator.translate(raw(kVkMultiply, 0), no_locks, 2.0, kCtrl);
    REQUIRE(virtual_key);
    CHECK(virtual_key->scan_code == -kVkMultiply);
    CHECK(virtual_key->injected());
    CHECK(virtual_key->os_modifiers == kCtrl);
    auto hardware = translator.translate(raw(kVkMultiply, 55), no_locks, 2.1, kCtrl);
    REQUIRE(hardware);
    CHECK_FALSE(hardware->os_modifiers);
}

TEST_CASE("the translator names a key its table misses once, as keyboard's lazy to_name does",
          "[hotkeys]") {
    // The Pause key reports scan code 69 with VK_PAUSE (19), a pair no layout mapping lists;
    // keyboard names it "pause" when the hook first sees it.
    constexpr int kVkPause = 0x13;
    REQUIRE(KeyNameTable::generated().name(69, kVkPause, false, 0).empty());
    int asked = 0;
    KeyEventTranslator translator(
        KeyNameTable::generated(),
        [&asked](int sc, int vk, bool, unsigned) -> std::optional<std::string> {
            ++asked;
            if (sc == 69 && vk == kVkPause) {
                return "pause";
            }
            return std::nullopt;
        });
    const LockState no_locks;

    auto pause = translator.translate(raw(kVkPause, 69), no_locks, 1.0, std::nullopt);
    REQUIRE(pause);
    CHECK(pause->name == "pause");
    CHECK_FALSE(pause->keypad);
    auto again = translator.translate(raw(kVkPause, 69, false), no_locks, 1.1, std::nullopt);
    REQUIRE(again);
    CHECK(again->name == "pause");
    CHECK(asked == 1);

    // A key the table names never asks; a key nobody names stays nameless and is asked once.
    CHECK(translator.translate(raw(kVkMultiply, 55), no_locks, 1.2, std::nullopt)->name == "*");
    CHECK(asked == 1);
    CHECK(translator.translate(raw(0xFF, 9999), no_locks, 1.3, std::nullopt)->name.empty());
    CHECK(translator.translate(raw(0xFF, 9999), no_locks, 1.4, std::nullopt)->name.empty());
    CHECK(asked == 2);

    // A shortcut on that key now matches.
    HotkeyMatcher matcher({{"record_toggle", "pause"}});
    auto press = translator.translate(raw(kVkPause, 69), no_locks, 2.0, std::nullopt);
    REQUIRE(press);
    const Decision decision = matcher.handle(*press);
    CHECK(decision.suppress);
    REQUIRE(decision.effects.size() == 1);
    CHECK(decision.effects[0].kind == Effect::Kind::RecordToggle);
}

TEST_CASE("raw hook events drive the default shortcuts end to end", "[hotkeys]") {
    auto fake = std::make_unique<FakeKeyboardHook>();
    FakeKeyboardHook* hook = fake.get();
    HotkeyManager manager({}, std::move(fake));
    manager.set_spawner([](std::function<void()> call) { call(); });
    std::vector<std::string> events;
    Callbacks callbacks;
    callbacks.on_record_toggle = [&events] {
        events.push_back("record_toggle");
    };
    callbacks.on_cancel = [&events] {
        events.push_back("cancel");
    };
    callbacks.on_status_update_auto_hide = [&events](const std::string& status) {
        events.push_back(status);
    };
    manager.set_callbacks(callbacks);
    CHECK(manager.backend_available());
    CHECK(manager.backend_name() == "keyboard");

    double t = 10.0;
    const auto send = [&](RawKey key) {
        return hook->send_raw(key, t += 0.5);
    };

    // Num * toggles recording and never reaches the app; the main-row "*" does.
    CHECK_FALSE(send(raw(kVkMultiply, 55)));
    CHECK_FALSE(send(raw(kVkMultiply, 55, false)));
    CHECK(send(raw(kVkLShift, 42)));
    CHECK(send(raw(0x38, 9)));
    CHECK(send(raw(0x38, 9, false)));
    CHECK(send(raw(kVkLShift, 42, false)));
    // Num - cancels.
    CHECK_FALSE(send(raw(0x6D, 74)));
    CHECK_FALSE(send(raw(0x6D, 74, false)));
    // Ctrl+Alt+Num * disables dictation; Num * then reaches the app.
    CHECK(send(raw(kVkLControl, 29)));
    CHECK(send(raw(kVkLMenu, 56)));
    CHECK_FALSE(send(raw(kVkMultiply, 55)));
    CHECK_FALSE(send(raw(kVkMultiply, 55, false)));
    CHECK(send(raw(kVkLMenu, 56, false)));
    CHECK(send(raw(kVkLControl, 29, false)));
    CHECK(send(raw(kVkMultiply, 55)));
    CHECK(send(raw(kVkMultiply, 55, false)));

    CHECK(events == std::vector<std::string>{"record_toggle", "cancel", "STT Disabled"});
}

// Section 4 rule 4: the hook callback runs where Windows times it out. Typing that matches no
// shortcut (most keystrokes) must not reach the heap; a press a shortcut claims still records
// its hold, as the report of the package says.
TEST_CASE("typing that matches no shortcut does not allocate on the hook path", "[hotkeys]") {
    auto fake = std::make_unique<FakeKeyboardHook>();
    FakeKeyboardHook* hook = fake.get();
    HotkeyManager manager({}, std::move(fake));
    manager.set_spawner([](std::function<void()> call) { call(); });
    double t = 10.0;
    // Catch2's assertions may allocate, so the results are checked after the guarded scope.
    std::array<bool, 10> passed{};
    const auto type_letters = [&] {
        std::size_t i = 0;
        for (const auto& [vk, sc] :
             {std::pair{0x41, 30}, std::pair{0x53, 31}, std::pair{0x44, 32}}) {
            passed[i++] = hook->send_raw(raw(vk, sc), t += 0.1);
            passed[i++] = hook->send_raw(raw(vk, sc, false), t += 0.1);
        }
        passed[i++] = hook->send_raw(raw(kVkLShift, 42), t += 0.1);
        passed[i++] = hook->send_raw(raw(0x38, 9), t += 0.1); // the main-row "*", not "kp *"
        passed[i++] = hook->send_raw(raw(0x38, 9, false), t += 0.1);
        passed[i++] = hook->send_raw(raw(kVkLShift, 42, false), t += 0.1);
    };
    type_letters(); // the reused name buffer grows once
    {
        fv::testkit::RealtimeGuard guard;
        type_letters();
    }
    for (const bool reached_app : passed) {
        CHECK(reached_app);
    }
}

TEST_CASE("a hook that cannot be installed leaves the manager without a backend", "[hotkeys]") {
    auto fake = std::make_unique<FakeKeyboardHook>();
    fake->fail_next_install("Global hotkeys are not supported on this platform.");
    HotkeyManager manager({}, std::move(fake));
    CHECK_FALSE(manager.backend_available());
    CHECK(manager.backend_name() == "unavailable");
    CHECK(manager.backend_error() == "Global hotkeys are not supported on this platform.");
}

#ifdef _WIN32
// Asks this thread's layout for every name (ToUnicode, GetKeyNameText, MapVirtualKey): no hook,
// no input. The committed table is the US layout's, so another layout has nothing to compare.
TEST_CASE("the layout's own names are the generated ones on the US layout", "[platform]") {
    wchar_t layout[KL_NAMELENGTH] = {};
    REQUIRE(GetKeyboardLayoutNameW(layout));
    if (std::wstring(layout) != L"00000409") {
        SKIP("this thread's keyboard layout is not the US layout the table was generated on");
    }
    const KeyNameTable native = layout_key_names();
    const auto expected = KeyNameTable::generated().entries();
    const auto actual = native.entries();
    CHECK(actual.size() == expected.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < std::min(actual.size(), expected.size()); ++i) {
        if (actual[i] != expected[i]) {
            if (++differing <= 10) {
                UNSCOPED_INFO("key " << expected[i].first << " generated '" << expected[i].second
                                     << "', this layout '" << actual[i].second << "'");
            }
        }
    }
    CHECK(differing == 0);
}

// keyboard's _winkeyboard.get_event_names on this layout, first answer normalized: what the hook
// falls back to for a key its table misses. No hook, no input.
TEST_CASE("the layout names a key the table misses as keyboard does on the US layout",
          "[platform]") {
    wchar_t layout[KL_NAMELENGTH] = {};
    REQUIRE(GetKeyboardLayoutNameW(layout));
    if (std::wstring(layout) != L"00000409") {
        SKIP("this thread's keyboard layout is not the US layout the table was generated on");
    }
    CHECK(layout_event_name(69, VK_PAUSE, false, 0) == "pause");
    CHECK(layout_event_name(55, VK_MULTIPLY, false, 0) == "*");
    CHECK(layout_event_name(541, VK_LCONTROL, false, 0) == "alt gr");
    CHECK(layout_event_name(29, VK_RCONTROL, true, 0) == "right ctrl");
}
#endif
