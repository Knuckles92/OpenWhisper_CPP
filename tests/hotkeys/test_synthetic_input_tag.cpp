// OpenWhisper_CPP's own keystrokes (the paste after a dictation, the copy that reads the selection)
// carry kSyntheticInputTag, and the hook passes them on unseen, as `keyboard` passes the events
// of its own send() (direct_callback returns before any hook while is_replaying is set). Keys
// injected by other programs go through the shortcuts like typing, as in OpenWhisper. Driven
// through FakeKeyboardHook, so no hook is installed and nothing is injected.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "base/input_tag.h"
#include "harness.h"
#include "hotkeys/hold_machine.h"
#include "hotkeys/hotkey_manager.h"
#include "hotkeys/key_event.h"
#include "hotkeys/testing/fake_keyboard_hook.h"

using namespace fv;
using namespace fv::hotkeys;
using fv::hotkeys::testing::Harness;
using fv::hotkeys::testing::windows_hotkeys;

namespace {

constexpr int kVkLControl = 0xA2;
constexpr int kScanCtrl = 29;
constexpr int kVkV = 0x56;
constexpr int kScanV = 47;
constexpr int kVkMultiply = 0x6A;
constexpr int kScanNumpadStar = 55;

// Where a key came from, as the hook's KBDLLHOOKSTRUCT tells it.
enum class Source { Device, OurPaste, OtherProgram };

RawKey raw(int vk, int scan_code, bool down, Source source = Source::Device) {
    RawKey key;
    key.vk = vk;
    key.scan_code = scan_code;
    key.down = down;
    key.injected = source != Source::Device;
    if (source == Source::OurPaste) {
        key.extra_info = kSyntheticInputTag;
    } else if (source == Source::OtherProgram) {
        key.extra_info = 0x1234;
    }
    return key;
}

// Ctrl+V in the order SyntheticKeys sends a chord; whether each key reached the focused app.
std::vector<bool> send_ctrl_v(FakeKeyboardHook& hook, double& t, Source source) {
    return {
        hook.send_raw(raw(kVkLControl, kScanCtrl, true, source), t += 0.01),
        hook.send_raw(raw(kVkV, kScanV, true, source), t += 0.01),
        hook.send_raw(raw(kVkV, kScanV, false, source), t += 0.01),
        hook.send_raw(raw(kVkLControl, kScanCtrl, false, source), t += 0.01),
    };
}

// The recording side of a HoldMachine fed by the record press and release callbacks, as the
// dictation runtime feeds it.
class RecordingHost final : public HoldMachine::Host {
public:
    bool is_recording() override { return recording; }
    bool capture_canceled() override { return false; }
    bool hands_free_latch() override { return false; }
    bool wait_until_recording(double) override { return recording; }
    bool perform(const HoldMachine::Output& output) override {
        actions.push_back(output.action);
        if (output.action == HoldMachine::Action::Start) {
            recording = true;
        } else if (output.action == HoldMachine::Action::Stop ||
                   output.action == HoldMachine::Action::Cancel) {
            recording = false;
        }
        return true;
    }

    bool recording = false;
    std::vector<HoldMachine::Action> actions;
};

} // namespace

TEST_CASE("a ctrl v shortcut does not fire on our own paste", "[synthetic_input_tag]") {
    Harness h(windows_hotkeys({{"paste_last_original", "ctrl+v"}}));
    h.record_events();
    double t = 10.0;

    // Our paste reaches the focused app whole, and no shortcut sees it.
    CHECK(send_ctrl_v(*h.hook, t, Source::OurPaste) == std::vector<bool>{true, true, true, true});
    CHECK(h.events.empty());
    CHECK(h.manager->with_matcher([](HotkeyMatcher& m) { return m.press_holds(); }).empty());

    // The same keys from the keyboard fire the shortcut, which swallows V and its release.
    CHECK(send_ctrl_v(*h.hook, t, Source::Device) == std::vector<bool>{true, false, false, true});
    CHECK(h.events == std::vector<std::string>{"paste_original"});

    // Another program's injected Ctrl+V is treated as typing, as `keyboard` treats it.
    h.events.clear();
    CHECK(send_ctrl_v(*h.hook, t, Source::OtherProgram) ==
          std::vector<bool>{true, false, false, true});
    CHECK(h.events == std::vector<std::string>{"paste_original"});
}

TEST_CASE("a tagged hook event skips the callback, an untagged injected one reaches it",
          "[synthetic_input_tag]") {
    Harness h(windows_hotkeys({{"paste_last_original", "ctrl+v"}}));
    h.record_events();
    h.held = {"ctrl"};

    KeyEvent ours = h.down("v", kScanV);
    ours.llkhf_injected = true;
    ours.extra_info = kSyntheticInputTag;
    CHECK(ours.own_input());
    CHECK(h.passes(ours));
    CHECK(h.events.empty());

    // The tag alone is not ours: input from a device is never injected.
    KeyEvent device = h.down("v", kScanV);
    device.extra_info = kSyntheticInputTag;
    CHECK_FALSE(device.own_input());
    CHECK_FALSE(h.passes(device));
    CHECK(h.events == std::vector<std::string>{"paste_original"});
    CHECK_FALSE(h.passes(h.up("v", kScanV)));

    KeyEvent other = h.down("v", kScanV);
    other.llkhf_injected = true;
    other.extra_info = kSyntheticInputTag ^ 1;
    CHECK_FALSE(other.own_input());
    CHECK_FALSE(h.passes(other));
    CHECK(h.events == std::vector<std::string>{"paste_original", "paste_original"});
}

TEST_CASE("our own keys leave the hold machine and the holds as they were",
          "[synthetic_input_tag]") {
    Harness h(windows_hotkeys());
    h.manager->set_record_mode(TriggerMode::PushHold);
    RecordingHost host;
    HoldMachine machine(host);
    Callbacks callbacks;
    callbacks.on_record_press = [&machine](double at) {
        machine.press(at);
    };
    callbacks.on_record_release = [&machine](double at) {
        machine.release(at);
    };
    h.manager->set_callbacks(callbacks);
    FakeKeyboardHook& hook = *h.hook;
    double t = 10.0;

    // The user holds the push-and-hold record key (Num *).
    CHECK_FALSE(hook.send_raw(raw(kVkMultiply, kScanNumpadStar, true), t += 0.01));
    REQUIRE(machine.state() == HoldMachine::State::Holding);
    const auto pressed_at = machine.press_time();
    const auto holds = [&h] {
        return h.manager->with_matcher(
            [](HotkeyMatcher& m) { return std::pair(m.record_key_held(), m.press_holds()); });
    };
    const auto holds_before = holds();
    REQUIRE(holds_before.first);
    REQUIRE(h.manager->keyboard_hold_active(t + 0.1));

    // OpenWhisper_CPP pastes meanwhile. Seen, its V would be the key that went down last and the
    // hold would no longer count as active; unseen, nothing moves.
    CHECK(send_ctrl_v(hook, t, Source::OurPaste) == std::vector<bool>{true, true, true, true});
    CHECK(holds() == holds_before);
    CHECK(h.manager->keyboard_hold_active(t + 0.1));
    CHECK(machine.state() == HoldMachine::State::Holding);
    CHECK(machine.press_time() == pressed_at);
    CHECK(host.actions == std::vector<HoldMachine::Action>{HoldMachine::Action::Start});
    CHECK_FALSE(hook.is_pressed(kScanV));
    CHECK(hook.pressed_modifiers() == 0);

    // Another program's paste is seen, as in OpenWhisper: V went down last now.
    send_ctrl_v(hook, t, Source::OtherProgram);
    CHECK_FALSE(h.manager->keyboard_hold_active(t + 0.1));
    CHECK(machine.state() == HoldMachine::State::Holding);

    // The release of the held key still ends the recording.
    CHECK_FALSE(hook.send_raw(raw(kVkMultiply, kScanNumpadStar, false), t += 1.0));
    CHECK(machine.state() == HoldMachine::State::Idle);
    CHECK(host.actions ==
          std::vector<HoldMachine::Action>{HoldMachine::Action::Start, HoldMachine::Action::Stop});
}

TEST_CASE("our own keys leave the pressed keys as they were", "[synthetic_input_tag]") {
    Harness h(windows_hotkeys());
    FakeKeyboardHook& hook = *h.hook;
    double t = 10.0;

    // The user holds Ctrl; our paste's Ctrl up does not release it in the hook's record, and its
    // V never counts as pressed.
    CHECK(hook.send_raw(raw(kVkLControl, kScanCtrl, true), t += 0.01));
    CHECK(hook.send_raw(raw(kVkV, kScanV, true, Source::OurPaste), t += 0.01));
    CHECK_FALSE(hook.is_pressed(kScanV));
    send_ctrl_v(hook, t, Source::OurPaste);
    CHECK(hook.pressed_modifiers() == kCtrl);
    CHECK(hook.is_pressed(kScanCtrl));
    CHECK_FALSE(hook.is_pressed(kScanV));

    // Another program's keys count as pressed and released, as `keyboard` records them.
    CHECK(hook.send_raw(raw(kVkV, kScanV, true, Source::OtherProgram), t += 0.01));
    CHECK(hook.is_pressed(kScanV));
    CHECK(hook.send_raw(raw(kVkLControl, kScanCtrl, false, Source::OtherProgram), t += 0.01));
    CHECK(hook.pressed_modifiers() == 0);
}
