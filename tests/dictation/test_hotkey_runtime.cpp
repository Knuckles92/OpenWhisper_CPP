#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <set>
#include <thread>

#include "base/testing/fake_clock.h"
#include "dictation_support.h"
#include "focus/capture_service.h"
#include "focus/synthetic_keys.h"
#include "focus/testing/fake_platform.h"
#include "hotkeys/defaults.h"
#include "hotkeys/grammar.h"
#include "hotkeys/key_names.h"
#include "hotkeys/matcher.h"
#include "hotkeys/testing/fake_mouse_hook.h"
#include "settings/hotkeys.h"

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::hotkeys::TriggerMode;

namespace {

using Calls = std::vector<std::string>;

// test_personalize_s6_backends._windows_hotkeys(): the stub defaults and the optional actions.
std::map<std::string, std::string>
windows_hotkeys(const std::map<std::string, std::string>& overrides = {}) {
    std::map<std::string, std::string> hotkeys{{"record_toggle", "kp *"},
                                               {"cancel", "kp -"},
                                               {"enable_disable", "ctrl+alt+kp *"},
                                               {"minimize_tray", "ctrl+alt+m"},
                                               {"meeting_toggle", ""},
                                               {"command_mode", "ctrl+alt+k"},
                                               {"scratchpad_toggle", "ctrl+alt+s"},
                                               {"cycle_language", "ctrl+alt+l"},
                                               {"paste_last_original", "ctrl+alt+o"}};
    for (const auto& [action, hotkey] : overrides) {
        hotkeys.insert_or_assign(action, hotkey);
    }
    return hotkeys;
}

// _wired_windows_manager: push-and-hold, the record press and release queued for the runtime.
void wire(Rig& rig) {
    rig.manager->set_record_mode(TriggerMode::PushHold);
    fv::hotkeys::Callbacks callbacks;
    callbacks.on_record_press = [&rig](double at) {
        rig.runtime->queue_record_press(at);
    };
    callbacks.on_record_release = [&rig](double at) {
        rig.runtime->queue_record_release(at);
    };
    rig.manager->set_callbacks(std::move(callbacks));
}

// The OS keyboard as the Paste original test patches synthetic_keys._modifier_probe: a modifier
// is down while `held` says so.
class HeldKeyboard final : public fv::focus::KeyboardPort {
public:
    std::size_t send(std::span<const fv::focus::KeyStroke> strokes) override {
        return strokes.size();
    }
    std::uint16_t scan_code(std::uint16_t) override { return 0; }
    bool reads_key_state() const override { return true; }
    bool key_down(std::uint16_t vk) override {
        return held.load() && (vk == fv::focus::VK_CONTROL_KEY || vk == fv::focus::VK_MENU_KEY);
    }
    std::atomic<bool> held{false};
};

// Logs its waits into the shared call log.
class LoggingKeys final : public fv::focus::SyntheticKeys {
public:
    explicit LoggingKeys(std::vector<std::string>& log, std::mutex& mutex)
        : log_(log), mutex_(mutex) {}
    void send_paste() override {}
    void send_copy() override {}
    bool modifiers_held() override { return false; }
    bool wait_for_modifiers_released(std::chrono::milliseconds, const fv::CancelToken&) override {
        {
            std::lock_guard lock(mutex_);
            log_.push_back("wait");
        }
        threads.push_back(std::this_thread::get_id());
        return true;
    }
    std::vector<std::thread::id> threads;

private:
    std::vector<std::string>& log_;
    std::mutex& mutex_;
};

} // namespace

// ---- tests/test_personalize_fix_f1_paste_original_keys.py ----

// from
// tests/test_personalize_fix_f1_paste_original_keys.py::test_the_shortcut_waits_for_its_modifiers_before_pasting
TEST_CASE("test_the_shortcut_waits_for_its_modifiers_before_pasting",
          "[test_personalize_fix_f1_paste_original_keys]") {
    auto keyboard = std::make_shared<HeldKeyboard>();
    fv::focus::NativeSyntheticKeys keys(keyboard);
    auto fake = std::make_unique<fv::hotkeys::FakeKeyboardHook>();
    auto* hook = fake.get();
    fv::hotkeys::HotkeyManager manager(windows_hotkeys(), std::move(fake));
    FakeRecordingControl control;
    fv::MemorySettingsStore settings;
    InlineExecutor ui;
    HotkeyRuntime runtime(manager, control, settings, ui, fv::system_clock(), keys);
    manager.set_callbacks({.on_paste_last_original = [&runtime] {
        runtime.paste_last_original_after_keys_up();
    }});
    std::vector<bool> held_at_paste;
    std::mutex mutex;
    control.on_paste = [&] {
        std::lock_guard lock(mutex);
        held_at_paste.push_back(keyboard->held.load());
    };

    keyboard->held = true;
    CHECK_FALSE(hook->send(key_event(true, "o", false, fv::hotkeys::kCtrl | fv::hotkeys::kAlt)));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(control.calls().empty());
    keyboard->held = false;

    CHECK(control.wait_for_calls(1));
    std::lock_guard lock(mutex);
    CHECK(held_at_paste == std::vector<bool>{false});
}

// from
// tests/test_personalize_fix_f1_paste_original_keys.py::test_keys_still_held_at_the_deadline_paste_nothing_and_say_why
TEST_CASE("test_keys_still_held_at_the_deadline_paste_nothing_and_say_why",
          "[test_personalize_fix_f1_paste_original_keys]") {
    Rig rig;
    rig.keys.released_by_default = false;

    rig.runtime->paste_last_original_after_keys_up();

    CHECK(rig.control.calls().empty());
    CHECK(rig.statuses->values() == std::vector<std::string>{std::string(KEYS_STILL_HELD)});
}

// from
// tests/test_personalize_fix_f1_paste_original_keys.py::test_setup_wires_the_shortcut_through_the_wait
TEST_CASE("test_setup_wires_the_shortcut_through_the_wait",
          "[test_personalize_fix_f1_paste_original_keys]") {
    std::vector<std::string> log;
    std::mutex mutex;
    LoggingKeys keys(log, mutex);
    auto fake = std::make_unique<fv::hotkeys::FakeKeyboardHook>();
    auto* hook = fake.get();
    fv::hotkeys::HotkeyManager manager({}, std::move(fake));
    manager.set_spawner([](std::function<void()> call) { call(); });
    FakeRecordingControl control;
    control.on_paste = [&] {
        std::lock_guard lock(mutex);
        log.push_back("paste");
    };
    fv::MemorySettingsStore settings(
        fv::Json{{"hotkeys", {{"paste_last_original", "ctrl+alt+o"}}}});
    InlineExecutor ui;
    HotkeyRuntime runtime(manager, control, settings, ui, fv::system_clock(), keys);

    runtime.setup_hotkeys();
    // Python calls the callback the manager was given; the key reaches it here.
    hook->send(key_event(true, "o", false, fv::hotkeys::kCtrl | fv::hotkeys::kAlt));

    std::lock_guard lock(mutex);
    CHECK(log == std::vector<std::string>{"wait", "paste"});
}

// from
// tests/test_personalize_fix_f1_paste_original_keys.py::test_the_wait_runs_on_the_press_thread_not_the_caller
TEST_CASE("test_the_wait_runs_on_the_press_thread_not_the_caller",
          "[test_personalize_fix_f1_paste_original_keys]") {
    // The manager spawns a thread per press; the wait blocks only that one.
    std::vector<std::string> log;
    std::mutex mutex;
    LoggingKeys keys(log, mutex);
    auto fake = std::make_unique<fv::hotkeys::FakeKeyboardHook>();
    auto* hook = fake.get();
    fv::hotkeys::HotkeyManager manager(windows_hotkeys(), std::move(fake));
    FakeRecordingControl control;
    fv::MemorySettingsStore settings;
    InlineExecutor ui;
    HotkeyRuntime runtime(manager, control, settings, ui, fv::system_clock(), keys);
    manager.set_callbacks({.on_paste_last_original = [&runtime] {
        runtime.paste_last_original_after_keys_up();
    }});

    hook->send(key_event(true, "o", false, fv::hotkeys::kCtrl | fv::hotkeys::kAlt));
    // The press's thread ends with the paste; wait for it so nothing outlives the test.
    REQUIRE(control.wait_for_calls(1));

    std::lock_guard lock(mutex);
    REQUIRE(keys.threads.size() == 1);
    CHECK(keys.threads.front() != std::this_thread::get_id());
}

// ---- The Paste walkthrough shortcut (hotkeys.py _paste_walkthrough_after_keys_up) ----

namespace {

// Installs a capture service for one test and puts the previous one back.
struct ScopedCaptureService {
    explicit ScopedCaptureService(std::shared_ptr<fv::focus::CaptureService> service)
        : previous(fv::focus::get_service()) {
        fv::focus::set_service(std::move(service));
    }
    ~ScopedCaptureService() { fv::focus::set_service(previous); }
    std::shared_ptr<fv::focus::CaptureService> previous;
};

fv::focus::AppIdentity notepad() {
    return fv::focus::AppIdentity{"notepad.exe", "Notepad", 7, "0x10", "", false, "windows"};
}

} // namespace

TEST_CASE("the walkthrough paste shortcut carries the app it was pressed in",
          "[dictation][hotkey_runtime]") {
    auto service = std::make_shared<fv::focus::FakeCaptureService>(fv::focus::FocusSnapshot{});
    service->set_current(notepad());
    ScopedCaptureService installed(service);
    Rig rig({});
    rig.manager->set_spawner([](std::function<void()> call) { call(); });
    // The app is read before the wait, so a focus change while the keys come up is not followed.
    rig.keys.released_answers.push_back(true);
    Emitted<std::optional<fv::focus::AppIdentity>> pasted(rig.runtime->paste_walkthrough_requested,
                                                          rig.inline_ui);
    rig.runtime->setup_hotkeys();

    // config.DEFAULT_HOTKEYS paste_walkthrough: ctrl+shift+f12.
    CHECK_FALSE(
        rig.hook->send(key_event(true, "f12", false, fv::hotkeys::kCtrl | fv::hotkeys::kShift)));

    REQUIRE(pasted.values().size() == 1);
    CHECK(pasted.values().front() == notepad());
    CHECK(rig.keys.waits.size() == 1);
    CHECK(rig.statuses->values().empty());
}

TEST_CASE("the walkthrough paste with the keys still held pastes nothing and says why",
          "[dictation][hotkey_runtime]") {
    auto service = std::make_shared<fv::focus::FakeCaptureService>(fv::focus::FocusSnapshot{});
    service->set_current(notepad());
    ScopedCaptureService installed(service);
    Rig rig({});
    rig.keys.released_by_default = false;
    Emitted<std::optional<fv::focus::AppIdentity>> pasted(rig.runtime->paste_walkthrough_requested,
                                                          rig.inline_ui);

    rig.runtime->paste_walkthrough_after_keys_up();

    CHECK(pasted.values().empty());
    CHECK(rig.statuses->values() ==
          std::vector<std::string>{std::string(WALKTHROUGH_KEYS_STILL_HELD)});
}

// ---- tests/test_hook_refresh_waits_for_hold.py ----

// from
// tests/test_hook_refresh_waits_for_hold.py::test_refresh_waits_for_the_held_record_key_then_runs
TEST_CASE("test_refresh_waits_for_the_held_record_key_then_runs",
          "[test_hook_refresh_waits_for_hold]") {
    Rig rig(windows_hotkeys());
    rig.manager->set_spawner([](std::function<void()> call) { call(); });
    wire(rig);
    CHECK_FALSE(rig.hook->send(key_event(true, "*")));
    rig.drain();

    const int installs = rig.hook->installs();
    rig.runtime->on_periodic_hook_refresh();
    CHECK(rig.hook->installs() == installs);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(rig.hook->send(key_event(false, "*")));
    rig.drain();
    CHECK(rig.control.calls() == Calls{"start", "stop"});

    rig.runtime->on_watchdog_tick();
    CHECK(rig.hook->installs() == installs + 1);
    CHECK_FALSE(rig.runtime->refresh_pending());
}

// from tests/test_hook_refresh_waits_for_hold.py::test_refresh_runs_when_nothing_is_held
TEST_CASE("test_refresh_runs_when_nothing_is_held", "[test_hook_refresh_waits_for_hold]") {
    Rig rig(windows_hotkeys());
    wire(rig);
    const int installs = rig.hook->installs();

    rig.runtime->on_periodic_hook_refresh();

    CHECK(rig.hook->installs() == installs + 1);
    CHECK_FALSE(rig.runtime->refresh_pending());
}

TEST_CASE("Wayland does not reinstall a platform keyboard hook") {
    Rig rig(windows_hotkeys(), nullptr, fv::system_clock(), true);
    wire(rig);
    const auto installs = rig.hook->installs();
    rig.runtime->on_periodic_hook_refresh();

    CHECK(rig.hook->installs() == installs);
    CHECK_FALSE(rig.runtime->refresh_pending());
}

// from tests/test_hook_refresh_waits_for_hold.py::test_refresh_waits_for_a_held_command_key
TEST_CASE("test_refresh_waits_for_a_held_command_key", "[test_hook_refresh_waits_for_hold]") {
    // Command Mode's own handling is M4's; _wire hands its press and release to the dispatcher,
    // which here only records them. The hold the refresh waits for is the manager's.
    Rig rig(windows_hotkeys({{"command_mode", "kp /"}}));
    rig.manager->set_record_mode(TriggerMode::PushHold);
    std::atomic<int> command_events{0};
    fv::hotkeys::Callbacks callbacks;
    callbacks.on_record_press = [&rig](double at) {
        rig.runtime->queue_record_press(at);
    };
    callbacks.on_record_release = [&rig](double at) {
        rig.runtime->queue_record_release(at);
    };
    callbacks.on_command_press = [&command_events](double) {
        ++command_events;
    };
    callbacks.on_command_release = [&command_events](double) {
        ++command_events;
    };
    rig.manager->set_callbacks(std::move(callbacks));
    CHECK_FALSE(rig.hook->send(key_event(true, "/")));

    const int installs = rig.hook->installs();
    rig.runtime->on_periodic_hook_refresh();
    CHECK(rig.hook->installs() == installs);
    CHECK_FALSE(rig.hook->send(key_event(false, "/")));
    rig.runtime->on_watchdog_tick();
    CHECK(rig.hook->installs() == installs + 1);
    CHECK(command_events == 2);
}

// ---- tests/test_personalize_fix2_g1_rehook_hold.py ----

// from
// tests/test_personalize_fix2_g1_rehook_hold.py::test_keyboard_hold_across_a_rehook_stops_the_recording
TEST_CASE("test_keyboard_hold_across_a_rehook_stops_the_recording",
          "[test_personalize_fix2_g1_rehook_hold]") {
    Rig rig(windows_hotkeys());
    wire(rig);
    CHECK_FALSE(rig.hook->send(key_event(true, "*")));
    rig.drain();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    rig.runtime->rehook_keyboard();
    // The key is still down, so Windows keeps repeating it.
    CHECK_FALSE(rig.hook->send(key_event(true, "*")));
    CHECK_FALSE(rig.hook->send(key_event(false, "*")));
    rig.drain();

    CHECK(rig.control.calls() == Calls{"start", "stop"});
}

// from
// tests/test_personalize_fix2_g1_rehook_hold.py::test_windows_side_button_hold_across_a_rehook_stops_the_recording
TEST_CASE("test_windows_side_button_hold_across_a_rehook_stops_the_recording",
          "[test_personalize_fix2_g1_rehook_hold]") {
    Rig rig(windows_hotkeys({{"record_toggle", "mouse4"}}));
    auto fake = std::make_unique<fv::hotkeys::FakeMouseHook>();
    auto* mouse = fake.get();
    rig.manager->set_mouse_hook_source(std::move(fake));
    wire(rig);
    REQUIRE(mouse->running());
    const double start = monotonic_now();
    mouse->set_now(start);
    CHECK(mouse->press(fv::hotkeys::MouseButton::Mouse4));
    rig.drain();
    rig.runtime->rehook_keyboard();
    REQUIRE(mouse->running());
    mouse->set_now(start + 2.0);
    CHECK(mouse->release(fv::hotkeys::MouseButton::Mouse4));
    rig.drain();

    CHECK(rig.control.calls() == Calls{"start", "stop"});
    CHECK_FALSE(rig.control.recording.load());
}

// from
// tests/test_personalize_fix2_g1_rehook_hold.py::test_lost_release_lets_the_next_press_and_release_stop_the_recording
TEST_CASE("test_lost_release_lets_the_next_press_and_release_stop_the_recording",
          "[test_personalize_fix2_g1_rehook_hold]") {
    Rig rig;
    const double start = monotonic_now();
    rig.runtime->queue_record_press(start);
    // The release never arrives; the user presses and lets go again.
    rig.runtime->queue_record_press(start + 5.0);
    rig.runtime->queue_record_release(start + 5.2);
    rig.drain();

    CHECK(rig.control.calls() == Calls{"start", "stop"});
    CHECK(rig.runtime->hold_machine().state() == fv::hotkeys::HoldMachine::State::Idle);
}

// from
// tests/test_personalize_fix2_g1_rehook_hold.py::test_press_during_another_recording_is_still_ignored
TEST_CASE("test_press_during_another_recording_is_still_ignored",
          "[test_personalize_fix2_g1_rehook_hold]") {
    Rig rig;
    const double start = monotonic_now();
    rig.runtime->queue_record_press(start);
    rig.runtime->queue_record_release(start + 1.0);
    rig.drain();
    // A recording started elsewhere (tray, profile) owns the recorder.
    rig.control.recording = true;
    rig.runtime->queue_record_press(start + 2.0);
    rig.runtime->queue_record_release(start + 3.0);
    rig.drain();

    CHECK(rig.control.calls() == Calls{"start", "stop"});
}

// ---- The runtime's own behaviour (A04-51, A04-53, A04-56, A04-59) ----

TEST_CASE("kp * reaches toggle on the UI executor", "[dictation][hotkey_runtime]") {
    fv::ThreadPool ui("test-ui", 1);
    Rig rig({}, &ui);
    rig.runtime->setup_hotkeys();

    CHECK_FALSE(rig.hook->send(key_event(true, "*")));
    CHECK_FALSE(rig.hook->send(key_event(false, "*")));

    REQUIRE(rig.control.wait_for_calls(1));
    CHECK(rig.control.calls() == Calls{"toggle"});
    CHECK(rig.control.on_ui() == std::vector<bool>{true});
}

TEST_CASE("holding kp * in push-and-hold starts and the release stops, on the UI executor",
          "[dictation][hotkey_runtime]") {
    fv::ThreadPool ui("test-ui", 1);
    Rig rig({}, &ui);
    rig.settings.update(fv::Json{{"recording_trigger_mode", "push_hold"}});
    rig.runtime->setup_hotkeys();

    auto down = key_event(true, "*");
    CHECK_FALSE(rig.hook->send(down));
    auto up = key_event(false, "*");
    up.t = down.t + 0.6;
    CHECK_FALSE(rig.hook->send(up));
    rig.drain();

    CHECK(rig.control.calls() == Calls{"start", "stop"});
    CHECK(rig.control.on_ui() == std::vector<bool>{true, true});
    CHECK(rig.control.sources() == std::vector<std::string>{"hotkey"});
}

TEST_CASE("a tap and a second press latch hands-free and say how to stop",
          "[dictation][hotkey_runtime]") {
    Rig rig({});
    rig.settings.update(fv::Json{{"recording_trigger_mode", "push_hold"}});
    rig.runtime->setup_hotkeys();
    const double t = monotonic_now();
    auto event = [&](bool down, double at) {
        auto e = key_event(down, "*");
        e.t = at;
        CHECK_FALSE(rig.hook->send(e));
    };

    event(true, t);
    event(false, t + 0.1);
    event(true, t + 0.25);
    event(false, t + 0.3);
    rig.drain();
    CHECK(rig.control.calls() == Calls{"start"});
    CHECK(rig.hands_free->values() == std::vector<bool>{true});
    CHECK(rig.statuses->values().back() ==
          std::format(
              "Hands-free · press {} to stop",
              fv::hotkeys::format_hotkey_display("kp *", fv::hotkeys::platform_vocabulary())));

    event(true, t + 3.0);
    event(false, t + 3.1);
    rig.drain();
    CHECK(rig.control.calls() == Calls{"start", "stop"});
    CHECK(rig.hands_free->values() == std::vector<bool>{true, false});
}

TEST_CASE("a short tap through the hook cancels when the latch is off",
          "[dictation][hotkey_runtime]") {
    Rig rig({});
    rig.settings.update(
        fv::Json{{"recording_trigger_mode", "push_hold"}, {"recording_hands_free_latch", false}});
    rig.runtime->setup_hotkeys();
    auto down = key_event(true, "*");
    CHECK_FALSE(rig.hook->send(down));
    auto up = key_event(false, "*");
    up.t = down.t + 0.1;
    CHECK_FALSE(rig.hook->send(up));
    rig.drain();

    CHECK(rig.control.calls() == Calls{"start", "cancel"});
    CHECK(rig.hands_free->values().empty());
}

TEST_CASE("while a shortcut is being captured the record key passes",
          "[dictation][hotkey_runtime]") {
    Rig rig({});
    rig.runtime->setup_hotkeys();
    rig.runtime->set_capture_suspended(true);
    CHECK(rig.manager->capture_suspended());
    CHECK(rig.hook->send(key_event(true, "*")));
    CHECK(rig.hook->send(key_event(false, "*")));
    rig.runtime->set_capture_suspended(false);
    CHECK_FALSE(rig.manager->capture_suspended());
    CHECK(rig.control.calls().empty());
}

TEST_CASE("setup merges the defaults under the saved hotkeys", "[dictation][hotkey_runtime]") {
    Rig rig({});
    rig.settings.update(fv::Json{
        {"hotkeys", {{"record_toggle", "f9"}, {"custom_action", "ctrl+alt+j"}, {"bad", 5}}}});
    Emitted<HotkeyRuntime::HotkeyMap> announced(rig.runtime->hotkeys_changed, rig.inline_ui);

    rig.runtime->setup_hotkeys();

    HotkeyRuntime::HotkeyMap expected;
    for (const auto& [action, hotkey] : fv::hotkeys::default_hotkeys()) {
        expected[action] = hotkey;
    }
    expected["record_toggle"] = "f9";
    expected["custom_action"] = "ctrl+alt+j";
    REQUIRE(announced.values().size() == 1);
    CHECK(announced.values().front() == expected);
    CHECK(rig.manager->hotkeys().at("record_toggle") == "f9");
    CHECK(rig.manager->hotkeys().at("cancel") == "kp -");
    CHECK(rig.manager->with_matcher([](auto& matcher) { return matcher.record_mode(); }) ==
          TriggerMode::Toggle);
}

TEST_CASE("update_hotkeys applies, saves as given and says so", "[dictation][hotkey_runtime]") {
    Rig rig({});
    Emitted<HotkeyRuntime::HotkeyMap> announced(rig.runtime->hotkeys_changed, rig.inline_ui);

    rig.runtime->update_hotkeys({{"record_toggle", "f8"}, {"cancel", "esc"}});

    CHECK(rig.manager->hotkeys().at("record_toggle") == "f8");
    CHECK(rig.settings.contents().at("hotkeys") ==
          fv::Json{{"record_toggle", "f8"}, {"cancel", "esc"}});
    CHECK(rig.settings.contents().at("hotkeys").begin().key() == "record_toggle");
    REQUIRE(announced.values().size() == 1);
    CHECK(announced.values().front() ==
          HotkeyRuntime::HotkeyMap{{"record_toggle", "f8"}, {"cancel", "esc"}});
    CHECK(rig.statuses->values() == std::vector<std::string>{"Hotkeys updated"});
}

TEST_CASE("the trigger mode switch says which mode is on", "[dictation][hotkey_runtime]") {
    Rig rig;
    rig.runtime->set_recording_trigger_mode("push_hold");
    rig.runtime->set_recording_trigger_mode("toggle");
    rig.runtime->set_recording_trigger_mode("");
    CHECK(rig.statuses->values() == std::vector<std::string>{"Push-and-hold recording enabled",
                                                             "Toggle recording enabled",
                                                             "Toggle recording enabled"});
}

TEST_CASE("the other shortcuts post to the UI executor", "[dictation][hotkey_runtime]") {
    fv::ThreadPool ui("test-ui", 1);
    Rig rig({}, &ui);
    rig.manager->set_spawner([](std::function<void()> call) { call(); });
    rig.settings.update(fv::Json{{"hotkeys", {{"scratchpad_toggle", "ctrl+alt+s"}}}});
    rig.runtime->setup_hotkeys();
    int scratchpads = 0;
    auto link =
        rig.runtime->scratchpad_toggle_requested.connect(rig.inline_ui, [&] { ++scratchpads; });

    CHECK_FALSE(rig.hook->send(key_event(true, "-")));
    rig.hook->send(key_event(false, "-"));
    CHECK_FALSE(
        rig.hook->send(key_event(true, "m", false, fv::hotkeys::kCtrl | fv::hotkeys::kAlt)));
    CHECK_FALSE(
        rig.hook->send(key_event(true, "s", false, fv::hotkeys::kCtrl | fv::hotkeys::kAlt)));

    REQUIRE(rig.control.wait_for_calls(2));
    CHECK(rig.control.calls() == Calls{"cancel", "minimize"});
    CHECK(rig.control.on_ui() == std::vector<bool>{true, true});
    CHECK(scratchpads == 1);
}

TEST_CASE("STT Enabled and Disabled reach the overlay", "[dictation][hotkey_runtime]") {
    Rig rig;
    rig.runtime->connect_signals();
    Emitted<bool> stt(rig.runtime->stt_state_changed, rig.inline_ui);
    Emitted<fv::appcore::OverlayState> overlay(rig.runtime->overlay_state_update, rig.inline_ui);

    rig.runtime->update_status_with_auto_hide("STT Disabled");
    rig.runtime->update_status_with_auto_hide("STT Enabled");
    rig.runtime->update_status_with_auto_hide("Recording...");

    CHECK(rig.statuses->values() ==
          std::vector<std::string>{"STT Disabled", "STT Enabled", "Recording..."});
    CHECK(stt.values() == std::vector<bool>{false, true});
    CHECK(overlay.values() ==
          std::vector<fv::appcore::OverlayState>{fv::appcore::OverlayState::SttDisabled,
                                                 fv::appcore::OverlayState::SttEnabled});
}

TEST_CASE("the enable/disable shortcut says STT Disabled through the hook",
          "[dictation][hotkey_runtime]") {
    Rig rig({});
    rig.runtime->connect_signals();
    rig.runtime->setup_hotkeys();
    Emitted<fv::appcore::OverlayState> overlay(rig.runtime->overlay_state_update, rig.inline_ui);

    CHECK_FALSE(rig.hook->send(key_event(true, "*", true, fv::hotkeys::kCtrl | fv::hotkeys::kAlt)));

    CHECK(rig.statuses->values() == std::vector<std::string>{"STT Disabled"});
    CHECK(overlay.values() ==
          std::vector<fv::appcore::OverlayState>{fv::appcore::OverlayState::SttDisabled});
}

TEST_CASE("the watchdog rehooks after a sleep and refreshes every five minutes",
          "[dictation][hotkey_runtime]") {
    fv::FakeClock clock;
    InlineExecutor ui;
    Rig rig({}, &ui, clock);
    rig.runtime->setup_hook_watchdog();
    CHECK(ui.pending_delays() == std::vector<std::chrono::milliseconds>{
                                     HOTKEY_WATCHDOG_INTERVAL, HOTKEY_HOOK_REFRESH_INTERVAL});
    const int installs = rig.hook->installs();

    // On time: nothing to do.
    clock.advance(std::chrono::seconds(10));
    rig.runtime->on_watchdog_tick();
    CHECK(rig.hook->installs() == installs);
    // 31 s late: the computer slept.
    clock.advance(std::chrono::seconds(41));
    rig.runtime->on_watchdog_tick();
    CHECK(rig.hook->installs() == installs + 1);
    // Exactly 30 s late is not a sleep.
    clock.advance(std::chrono::seconds(40));
    rig.runtime->on_watchdog_tick();
    CHECK(rig.hook->installs() == installs + 1);

    // A periodic refresh within 60 s of the last rehook is skipped.
    clock.advance(std::chrono::seconds(10));
    rig.runtime->on_periodic_hook_refresh();
    CHECK(rig.hook->installs() == installs + 1);
    clock.advance(std::chrono::seconds(60));
    rig.runtime->on_periodic_hook_refresh();
    CHECK(rig.hook->installs() == installs + 2);

    // The timers re-arm themselves.
    CHECK(ui.fire_timers() == 2);
    CHECK(ui.pending_delays().size() == 2);
    rig.runtime->cleanup();
    CHECK(ui.pending_delays().empty());
}
