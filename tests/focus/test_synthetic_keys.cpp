// Ports the Windows parts of tests/test_personalize_s3_synthetic_keys.py and the modifier-state
// tests of tests/test_personalize_fix_f2_selection_keys_held.py, and replays the
// services.synthetic_keys golden vectors. Keystrokes go to a fake KeyboardPort; time is a
// FakeClock that the injected sleep moves.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <deque>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "base/input_tag.h"
#include "base/testing/fake_clock.h"
#include "focus/synthetic_keys.h"
#include "focus/testing/fake_synthetic_keys.h"
#include "focus_support.h"
#include "golden/golden.h"

#ifdef _WIN32
#include "focus/win/send_input.h"
#endif

using namespace fv;
using namespace fv::focus;
using namespace std::chrono_literals;

namespace {

AppIdentity app(std::string app_id, std::string name = "", std::string platform = "windows") {
    return AppIdentity{
        .app_id = std::move(app_id), .name = std::move(name), .platform = std::move(platform)};
}

// Python's _Clock: time moves only when the code under test sleeps.
struct SleepingClock {
    FakeClock clock;
    std::vector<std::chrono::milliseconds> sleeps;

    CancellableSleep sleep() {
        return [this](std::chrono::milliseconds pause) {
            sleeps.push_back(pause);
            clock.advance(pause);
            return false;
        };
    }
    std::chrono::milliseconds slept() const {
        std::chrono::milliseconds total{0};
        for (const auto pause : sleeps) {
            total += pause;
        }
        return total;
    }
};

class FakeKeyboard final : public KeyboardPort {
public:
    std::size_t send(std::span<const KeyStroke> strokes) override {
        sent.insert(sent.end(), strokes.begin(), strokes.end());
        return accept_all ? strokes.size() : 0;
    }
    std::uint16_t scan_code(std::uint16_t vk) override {
        switch (vk) {
        case VK_CONTROL_KEY:
            return 0x1D;
        case 0x43:
            return 0x2E;
        case 0x56:
            return 0x2F;
        default:
            return 0;
        }
    }
    bool reads_key_state() const override { return readable; }
    bool key_down(std::uint16_t vk) override {
        reads.push_back(vk);
        return down.contains(vk);
    }

    std::vector<KeyStroke> sent;
    std::set<std::uint16_t> down;
    std::vector<std::uint16_t> reads;
    bool readable = true;
    bool accept_all = true;
};

// The OS key state agreeing with a set of held modifier names.
std::function<bool(std::uint16_t)> os_agrees(const std::set<std::string>& pressed) {
    return [&pressed](std::uint16_t vk) {
        return (vk == VK_CONTROL_KEY && pressed.contains("ctrl")) ||
               (vk == VK_MENU_KEY && pressed.contains("alt")) ||
               (vk == VK_SHIFT_KEY && pressed.contains("shift")) ||
               ((vk == VK_LWIN_KEY || vk == VK_RWIN_KEY) && pressed.contains("windows"));
    };
}

HookKeyState hook_of(const std::set<std::string>& pressed) {
    return [&pressed](std::string_view name) {
        return pressed.contains(std::string(name));
    };
}

} // namespace

// from tests/test_personalize_s3_synthetic_keys.py::test_terminals_never_get_a_copy
TEST_CASE("terminals never get a copy", "[test_personalize_s3_synthetic_keys]") {
    const auto identity = GENERATE(
        app("WindowsTerminal.exe", "Windows Terminal"), app("conhost.exe", "Console"),
        app("OpenConsole.exe"), app("powershell.exe", "Windows PowerShell"), app("pwsh.exe"),
        app("cmd.exe", "Command Prompt"), app("wezterm-gui.exe"), app("alacritty"),
        app("kitty", "", "hyprland"), app("com.mitchellh.ghostty", "", "hyprland"),
        app("foot", "", "hyprland"), app("com.googlecode.iterm2", "iTerm2", "macos"),
        app("com.apple.Terminal", "Terminal", "macos"));
    CAPTURE(identity);
    CHECK(is_terminal(identity));
    CHECK_FALSE(copies_line_without_selection(identity));
}

// from tests/test_personalize_s3_synthetic_keys.py::test_editors_that_copy_the_line_are_known
TEST_CASE("editors that copy the line are known", "[test_personalize_s3_synthetic_keys]") {
    const auto identity = GENERATE(
        app("Code.exe", "Visual Studio Code"), app("Cursor.exe", "Cursor"), app("Windsurf.exe"),
        app("devenv.exe", "Visual Studio"), app("idea64.exe", "IntelliJ IDEA"),
        app("pycharm64.exe", "PyCharm"), app("rider64.exe"), app("studio64.exe", "Android Studio"),
        app("sublime_text.exe", "Sublime Text"), app("zed", "", "linux"),
        app("jetbrains-pycharm", "", "hyprland"), app("com.jetbrains.goland", "GoLand", "macos"),
        app("com.microsoft.VSCode", "Code", "macos"));
    CAPTURE(identity);
    CHECK(copies_line_without_selection(identity));
    CHECK_FALSE(is_terminal(identity));
}

// from tests/test_personalize_s3_synthetic_keys.py::test_ordinary_apps_are_neither
TEST_CASE("ordinary apps are neither", "[test_personalize_s3_synthetic_keys]") {
    const auto identity = GENERATE(std::optional<AppIdentity>(),
                                   std::optional<AppIdentity>(app("outlook.exe", "Outlook")),
                                   std::optional<AppIdentity>(app("chrome.exe", "Google Chrome")),
                                   std::optional<AppIdentity>(app("notepad.exe", "Notepad")));
    CAPTURE(test::describe(identity));
    CHECK_FALSE(is_terminal(identity));
    CHECK_FALSE(copies_line_without_selection(identity));
}

// from tests/test_personalize_s3_synthetic_keys.py::test_powershell_ise_is_an_editor_not_a_terminal
TEST_CASE("powershell ise is an editor not a terminal", "[test_personalize_s3_synthetic_keys]") {
    // Its console pane runs commands, but the window is a script editor, so a copy is safe to
    // send there.
    CHECK_FALSE(is_terminal(app("powershell_ise.exe", "Windows PowerShell ISE")));
}

// from tests/test_personalize_s3_synthetic_keys.py::test_the_app_catalogue_surface_counts_too
TEST_CASE("the app catalogue surface counts too", "[test_personalize_s3_synthetic_keys]") {
    const SurfaceLookup surfaces = [](const AppIdentity& identity) -> std::string {
        if (identity.app_id == "warpish") {
            return "terminal";
        }
        if (identity.app_id == "ide") {
            return "code";
        }
        return "text";
    };
    CHECK(is_terminal(app("warpish"), surfaces));
    CHECK(copies_line_without_selection(app("ide"), surfaces));
    CHECK_FALSE(is_terminal(app("ide"), surfaces));
}

// from
// tests/test_personalize_s3_synthetic_keys.py::test_a_broken_catalogue_or_identity_never_raises
TEST_CASE("a broken catalogue or identity never raises", "[test_personalize_s3_synthetic_keys]") {
    const SurfaceLookup broken = [](const AppIdentity&) -> std::string {
        throw std::runtime_error("catalogue failed");
    };
    // A typed AppIdentity cannot hold Python's None id or numeric name; an empty one stands in.
    const AppIdentity weird{};
    CHECK_FALSE(is_terminal(weird, broken));
    CHECK_FALSE(copies_line_without_selection(weird, broken));
    CHECK_FALSE(is_terminal(app("outlook.exe"), broken));
}

// from tests/test_personalize_s3_synthetic_keys.py::test_waits_until_the_modifiers_come_up
TEST_CASE("waits until the modifiers come up", "[test_personalize_s3_synthetic_keys]") {
    SleepingClock time;
    std::deque<bool> held{true, true, true, false};
    const auto probe = [&] {
        const bool value = held.front();
        held.pop_front();
        return value;
    };

    CHECK(wait_until_released(probe, 800ms, time.clock, time.sleep()));
    CHECK(time.sleeps == std::vector<std::chrono::milliseconds>(3, MODIFIER_POLL));
}

// from tests/test_personalize_s3_synthetic_keys.py::test_gives_up_at_the_timeout
TEST_CASE("gives up at the timeout", "[test_personalize_s3_synthetic_keys]") {
    SleepingClock time;

    CHECK_FALSE(wait_until_released([] { return true; }, 50ms, time.clock, time.sleep()));
    CHECK(time.slept() >= 50ms);
    CHECK(time.slept() <= 50ms + MODIFIER_POLL * 3 / 2);
}

// from
// tests/test_personalize_s3_synthetic_keys.py::test_unreadable_key_state_waits_briefly_and_counts_as_released
TEST_CASE("unreadable key state waits briefly and counts as released",
          "[test_personalize_s3_synthetic_keys]") {
    SleepingClock time;
    CHECK(wait_until_released({}, 800ms, time.clock, time.sleep()));
    CHECK(time.sleeps == std::vector<std::chrono::milliseconds>{BLIND_WAIT});

    const auto broken = []() -> bool {
        throw std::runtime_error("hook gone");
    };
    CHECK(wait_until_released(broken, 800ms, time.clock, time.sleep()));
}

// from
// tests/test_personalize_s3_synthetic_keys.py::test_windows_reads_every_modifier_from_the_keyboard_hook
TEST_CASE("windows reads every modifier from the keyboard hook",
          "[test_personalize_s3_synthetic_keys]") {
    std::set<std::string> pressed;
    // The OS agrees with the hook; never the real keyboard's state.
    const auto physical = os_agrees(pressed);
    const auto hook = hook_of(pressed);

    CHECK_FALSE(windows_modifiers_held(physical, hook));
    for (const std::string name : {"ctrl", "alt", "shift", "windows"}) {
        CAPTURE(name);
        pressed = {name};
        CHECK(windows_modifiers_held(physical, hook));
    }
}

// from tests/test_personalize_s3_synthetic_keys.py::test_windows_copy_is_ctrl_c
TEST_CASE("windows copy is ctrl c", "[test_personalize_s3_synthetic_keys]") {
    auto keyboard = std::make_shared<FakeKeyboard>();
    NativeSyntheticKeys keys(keyboard);

    keys.send_copy();

    CHECK(keyboard->sent == std::vector<KeyStroke>{{VK_CONTROL_KEY, 0x1D, false},
                                                   {0x43, 0x2E, false},
                                                   {0x43, 0x2E, true},
                                                   {VK_CONTROL_KEY, 0x1D, true}});
}

// --- tests/test_personalize_fix_f2_selection_keys_held.py -------------------------------------

// from
// tests/test_personalize_fix_f2_selection_keys_held.py::test_a_modifier_the_hook_thinks_is_stuck_but_is_up_counts_as_released
TEST_CASE("a modifier the hook thinks is stuck but is up counts as released",
          "[test_personalize_fix_f2_selection_keys_held]") {
    // The hook can miss a key-up that went to an elevated window; refusing on that alone would
    // block Command Mode until the key is pressed again.
    const std::set<std::string> pressed{"alt"};
    CHECK_FALSE(windows_modifiers_held([](std::uint16_t) { return false; }, hook_of(pressed)));
}

// from
// tests/test_personalize_fix_f2_selection_keys_held.py::test_a_modifier_the_os_also_holds_counts_as_held
TEST_CASE("a modifier the os also holds counts as held",
          "[test_personalize_fix_f2_selection_keys_held]") {
    const std::set<std::string> pressed{"windows"};
    std::vector<std::uint16_t> down;
    const auto physical = [&down](std::uint16_t vk) {
        down.push_back(vk);
        return vk == VK_RWIN_KEY;
    };

    CHECK(windows_modifiers_held(physical, hook_of(pressed)));
    CHECK(std::ranges::find(down, VK_RWIN_KEY) != down.end());
}

// from
// tests/test_personalize_fix_f2_selection_keys_held.py::test_without_the_os_key_state_the_hook_decides
TEST_CASE("without the os key state the hook decides",
          "[test_personalize_fix_f2_selection_keys_held]") {
    const std::set<std::string> pressed{"ctrl"};
    CHECK(windows_modifiers_held({}, hook_of(pressed)));
}

// --- sequencing and wiring --------------------------------------------------------------------

TEST_CASE("paste is ctrl v in chord order", "[test_personalize_s3_synthetic_keys]") {
    auto keyboard = std::make_shared<FakeKeyboard>();
    NativeSyntheticKeys keys(keyboard);

    keys.send_paste();
    keyboard->accept_all = false;
    CHECK_NOTHROW(keys.send_paste());

    const std::vector<KeyStroke> chord{{VK_CONTROL_KEY, 0x1D, false},
                                       {0x56, 0x2F, false},
                                       {0x56, 0x2F, true},
                                       {VK_CONTROL_KEY, 0x1D, true}};
    std::vector<KeyStroke> twice = chord;
    twice.insert(twice.end(), chord.begin(), chord.end());
    CHECK(keyboard->sent == twice);
}

TEST_CASE("native keys read the os modifier state", "[test_personalize_s3_synthetic_keys]") {
    auto keyboard = std::make_shared<FakeKeyboard>();
    std::set<std::string> hooked{"shift"};
    SleepingClock time;
    NativeSyntheticKeys keys(keyboard, hook_of(hooked), &time.clock, time.sleep());

    // The OS says nothing is down: the hook's stuck Shift does not count.
    CHECK_FALSE(keys.modifiers_held());
    CHECK(keys.wait_for_modifiers_released(800ms));
    CHECK(time.sleeps.empty());

    keyboard->down = {VK_LWIN_KEY};
    CHECK(keys.modifiers_held());
    CHECK_FALSE(keys.wait_for_modifiers_released(30ms));
    CHECK(time.slept() >= 30ms);

    // Where the OS state can't be read, the hook decides; with neither, the wait is blind.
    keyboard->readable = false;
    CHECK(keys.modifiers_held());
    NativeSyntheticKeys blind(keyboard, {}, &time.clock, time.sleep());
    CHECK_FALSE(blind.modifiers_held());
    time.sleeps.clear();
    CHECK(blind.wait_for_modifiers_released(800ms));
    CHECK(time.sleeps == std::vector<std::chrono::milliseconds>{BLIND_WAIT});
}

TEST_CASE("a cancelled wait gives up", "[test_personalize_s3_synthetic_keys]") {
    auto keyboard = std::make_shared<FakeKeyboard>();
    keyboard->down = {VK_CONTROL_KEY};
    CancelSource source;
    source.cancel();
    NativeSyntheticKeys keys(keyboard);
    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(keys.wait_for_modifiers_released(5000ms, source.token()));
    CHECK(std::chrono::steady_clock::now() - started < 2000ms);
}

TEST_CASE("native modifiers name what the os holds", "[test_personalize_s3_synthetic_keys]") {
    CHECK_FALSE(native_windows_modifiers({}));
    const std::set<std::string> pressed{"alt", "windows"};
    CHECK(native_windows_modifiers(os_agrees(pressed)) == pressed);
}

TEST_CASE("the fake synthetic keys record and script", "[test_personalize_s3_synthetic_keys]") {
    FakeSyntheticKeys keys;
    keys.send_paste();
    keys.send_copy();
    CHECK(keys.sent() == std::vector<std::string>{"paste", "copy"});
    keys.released_answers = {false};
    CHECK_FALSE(keys.wait_for_modifiers_released());
    CHECK(keys.wait_for_modifiers_released(100ms));
    CHECK(keys.waits == std::vector<std::chrono::milliseconds>{MODIFIER_WAIT, 100ms});
    keys.fail = true;
    CHECK_THROWS_AS(keys.send_paste(), SyntheticKeysError);
}

#ifdef _WIN32
TEST_CASE("the windows keyboard maps scan codes",
          "[test_personalize_s3_synthetic_keys][platform]") {
    const auto keyboard = make_keyboard_port();
    // Ctrl sits at one scan code on every layout; where C and V are depends on the layout
    // (QWERTY and AZERTY 0x2E and 0x2F, Dvorak elsewhere), so only that they map.
    CHECK(keyboard->scan_code(VK_CONTROL_KEY) == 0x1D);
    const auto c = keyboard->scan_code(0x43);
    const auto v = keyboard->scan_code(0x56);
    CHECK(c != 0);
    CHECK(v != 0);
    CHECK(c != v);
    CHECK(keyboard->reads_key_state());
}

// The INPUTs reach a recorder in place of SendInput, so nothing is injected; only the scan codes
// come from the OS (MapVirtualKeyW reads the layout).
TEST_CASE("every input the synthetic keys send carries our tag",
          "[test_personalize_s3_synthetic_keys][platform]") {
    std::vector<INPUT> sent;
    const auto port = make_keyboard_port([&sent](std::span<INPUT> inputs) {
        sent.insert(sent.end(), inputs.begin(), inputs.end());
        return static_cast<UINT>(inputs.size());
    });
    NativeSyntheticKeys keys(port);

    keys.send_paste();
    keys.send_copy();

    const WORD ctrl = VK_CONTROL_KEY;
    const std::vector<std::pair<WORD, bool>> expected{
        {ctrl, false}, {0x56, false}, {0x56, true}, {ctrl, true},
        {ctrl, false}, {0x43, false}, {0x43, true}, {ctrl, true},
    };
    REQUIRE(sent.size() == expected.size());
    for (std::size_t i = 0; i < sent.size(); ++i) {
        CAPTURE(i);
        CHECK(sent[i].type == INPUT_KEYBOARD);
        CHECK(sent[i].ki.dwExtraInfo == kSyntheticInputTag);
        CHECK(sent[i].ki.wVk == expected[i].first);
        CHECK(sent[i].ki.wScan == port->scan_code(expected[i].first));
        CHECK(sent[i].ki.dwFlags == (expected[i].second ? KEYEVENTF_KEYUP : 0u));
    }
}

// MapVirtualKeyW gives the chord's scan codes.
TEST_CASE("the keyboard port reports what the os took",
          "[test_personalize_s3_synthetic_keys][platform]") {
    std::size_t batches = 0;
    const auto port = make_keyboard_port([&batches](std::span<INPUT>) {
        ++batches;
        return UINT{1};
    });
    const std::vector<KeyStroke> chord = chord_strokes(*port, VK_CONTROL_KEY, 0x56);
    CHECK(port->send(chord) == 1);
    CHECK(batches == 1);
}

TEST_CASE("keyboard and mouse inputs are tagged on the way out",
          "[test_personalize_s3_synthetic_keys]") {
    std::vector<INPUT> batch(3);
    batch[0].type = INPUT_KEYBOARD;
    batch[0].ki.dwExtraInfo = 7; // whatever a caller left there is replaced
    batch[1].type = INPUT_MOUSE;
    batch[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    batch[2].type = INPUT_HARDWARE;
    batch[2].hi.uMsg = 0x1234;

    std::vector<INPUT> sent;
    const UINT taken = send_tagged(batch, [&sent](std::span<INPUT> inputs) {
        sent.assign(inputs.begin(), inputs.end());
        return static_cast<UINT>(inputs.size());
    });

    CHECK(taken == 3);
    REQUIRE(sent.size() == 3);
    CHECK(sent[0].ki.dwExtraInfo == kSyntheticInputTag);
    CHECK(sent[1].mi.dwExtraInfo == kSyntheticInputTag);
    CHECK(sent[1].mi.dwFlags == MOUSEEVENTF_LEFTDOWN);
    // A hardware INPUT has no extra info to carry the tag.
    CHECK(sent[2].hi.uMsg == 0x1234u);

    const std::vector<KeyStroke> strokes{{VK_SHIFT_KEY, 0x2A, false}, {VK_SHIFT_KEY, 0x2A, true}};
    for (const INPUT& input : keyboard_inputs(strokes)) {
        CHECK(input.type == INPUT_KEYBOARD);
        CHECK(input.ki.dwExtraInfo == kSyntheticInputTag);
    }
}

// Injects a Shift tap into whatever has focus.
TEST_CASE("the windows keyboard injects keystrokes",
          "[test_personalize_s3_synthetic_keys][interactive]") {
    const auto keyboard = make_keyboard_port();
    const std::vector<KeyStroke> tap{{VK_SHIFT_KEY, keyboard->scan_code(VK_SHIFT_KEY), false},
                                     {VK_SHIFT_KEY, keyboard->scan_code(VK_SHIFT_KEY), true}};
    CHECK(keyboard->send(tap) == tap.size());
}
#endif

// --- golden vectors ---------------------------------------------------------------------------

TEST_CASE("golden synthetic_keys is_terminal", "[golden]") {
    golden::for_each("services.synthetic_keys", "is_terminal", [](const golden::Case& c) {
        CHECK(is_terminal(test::identity_from(c.args()[0])) == c.result<bool>());
    });
}

TEST_CASE("golden copies_line_without_selection", "[golden]") {
    golden::for_each("services.synthetic_keys", "copies_line_without_selection",
                     [](const golden::Case& c) {
                         CHECK(copies_line_without_selection(test::identity_from(c.args()[0])) ==
                               c.result<bool>());
                     });
}
