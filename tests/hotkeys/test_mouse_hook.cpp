#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "base/metrics.h"
#include "harness.h"
#include "hotkeys/mouse_shortcuts.h"
#include "testkit/testkit.h"

using namespace fv::hotkeys;
using fv::hotkeys::testing::Harness;
using fv::hotkeys::testing::windows_hotkeys;

namespace {

using Events = std::vector<std::pair<std::string, double>>;

// test_personalize_fix2_g1_rehook_hold._windows_events: push-and-hold, every stamped callback
// recorded with its time.
struct StampedHarness {
    explicit StampedHarness(const std::map<std::string, std::string>& overrides)
        : h(windows_hotkeys(overrides)) {
        h.attach_mouse();
        h.manager->set_record_mode(TriggerMode::PushHold);
        Callbacks callbacks;
        callbacks.on_record_press = [this](double at) {
            events.emplace_back("press", at);
        };
        callbacks.on_record_release = [this](double at) {
            events.emplace_back("release", at);
        };
        callbacks.on_command_press = [this](double at) {
            events.emplace_back("command_press", at);
        };
        callbacks.on_command_release = [this](double at) {
            events.emplace_back("command_release", at);
        };
        h.manager->set_callbacks(std::move(callbacks));
    }

    void button(MouseButton button, bool pressed, std::uint8_t modifiers, double at) {
        h.manager->on_mouse_button({button, pressed, modifiers, at});
    }

    Harness h;
    Events events;
};

// test_personalize_s6_backends._HookHarness: the hook callback over a fake user32 whose
// GetAsyncKeyState reports `pressed`, asking a claims that records each question.
struct HookHarness {
    explicit HookHarness(std::set<MouseButton> claimed_buttons = {MouseButton::Mouse4,
                                                                  MouseButton::Mouse5})
        : claimed(std::move(claimed_buttons)), filter([this] {
              timer += 0.00025;
              return timer;
          }) {}

    bool send(unsigned message, unsigned xbutton = 1, std::uint32_t flags = 0, int code = 0) {
        return filter.decide(
            code, message, static_cast<std::uint32_t>(xbutton) << 16, flags,
            [this](int vk) -> short { return pressed.contains(vk) ? short{-32768} : short{0}; },
            [this](MouseButton button, std::uint8_t modifiers) {
                asked.emplace_back(button, modifiers);
                if (raise) {
                    throw std::runtime_error("claims failed");
                }
                return claimed.contains(button);
            },
            [this] {
                now += 0.01;
                return now;
            },
            [this](const MouseEvent& event) { queued.push_back(event); });
    }

    std::set<MouseButton> claimed;
    std::set<int> pressed;
    std::vector<std::pair<MouseButton, std::uint8_t>> asked;
    std::vector<MouseEvent> queued;
    bool raise = false;
    double now = 0;
    double timer = 0;
    MouseButtonFilter filter;
};

} // namespace

// Windows backend, against a fake mouse hook

// from
// tests/test_personalize_s6_backends.py::test_windows_mouse_hook_runs_only_while_a_side_button_is_bound
TEST_CASE("test_windows_mouse_hook_runs_only_while_a_side_button_is_bound",
          "[test_personalize_s6_backends]") {
    Harness h(windows_hotkeys());
    FakeMouseHook* hook = h.attach_mouse();
    CHECK_FALSE(hook->running());

    h.manager->update_hotkeys({{"scratchpad_toggle", "mouse4"}});
    CHECK(hook->running());
    h.manager->set_capture_suspended(true);
    CHECK_FALSE(hook->running());
    h.manager->set_capture_suspended(false);
    CHECK(hook->running());
    const int starts = hook->starts();
    h.manager->rehook();
    CHECK(hook->running());
    CHECK(hook->starts() == starts + 1);
    h.manager->update_hotkeys({{"scratchpad_toggle", ""}});
    CHECK_FALSE(hook->running());

    h.manager->set_dynamic_hotkeys("transform", {{"polish", "ctrl+mouse5"}},
                                   [](const std::string&) {});
    CHECK(hook->running());
    h.manager->cleanup();
    CHECK_FALSE(hook->running());
}

// from
// tests/test_personalize_s6_backends.py::test_windows_side_button_claims_follow_bindings_and_enabled_state
TEST_CASE("test_windows_side_button_claims_follow_bindings_and_enabled_state",
          "[test_personalize_s6_backends]") {
    Harness h(windows_hotkeys({{"record_toggle", "mouse4"}, {"enable_disable", "ctrl+mouse5"}}));
    h.attach_mouse();
    const auto claims = [&h](MouseButton button, std::uint8_t modifiers) {
        return h.manager->claims_mouse_button(button, modifiers);
    };
    CHECK(claims(MouseButton::Mouse4, 0));
    CHECK_FALSE(claims(MouseButton::Mouse4, kCtrl));
    CHECK_FALSE(claims(MouseButton::Mouse5, 0));
    h.manager->set_program_enabled(false);
    CHECK_FALSE(claims(MouseButton::Mouse4, 0));
    CHECK(claims(MouseButton::Mouse5, kCtrl));
    h.manager->set_program_enabled(true);
    h.manager->set_capture_suspended(true);
    CHECK_FALSE(claims(MouseButton::Mouse4, 0));
}

// from tests/test_personalize_s6_backends.py::test_windows_side_button_drives_push_and_hold
TEST_CASE("test_windows_side_button_drives_push_and_hold", "[test_personalize_s6_backends]") {
    StampedHarness s({{"record_toggle", "mouse4"}});
    s.button(MouseButton::Mouse4, true, 0, 3.0);
    s.button(MouseButton::Mouse4, true, 0, 3.05);
    s.button(MouseButton::Mouse4, false, 0, 3.4);
    CHECK(s.events == Events{{"press", 3.0}, {"release", 3.4}});
}

// Windows mouse hook, against a fake user32

// from
// tests/test_personalize_s6_backends.py::test_mouse_hook_passes_everything_but_side_buttons_untouched
TEST_CASE("test_mouse_hook_passes_everything_but_side_buttons_untouched",
          "[test_personalize_s6_backends]") {
    HookHarness harness;
    for (const unsigned message : {0x0200U, 0x0201U, 0x0202U, 0x020AU}) {
        CHECK_FALSE(harness.send(message));
    }
    CHECK(harness.asked.empty());
    CHECK(harness.queued.empty());
    CHECK(harness.filter.worst_callback_ms() == 0);
}

// from
// tests/test_personalize_s6_backends.py::test_mouse_hook_suppresses_a_claimed_press_and_always_its_release
TEST_CASE("test_mouse_hook_suppresses_a_claimed_press_and_always_its_release",
          "[test_personalize_s6_backends]") {
    HookHarness harness;
    harness.pressed = {kVkControl};
    CHECK(harness.send(kWmXButtonDown, 2));
    harness.claimed.clear(); // the binding goes away mid-press
    CHECK(harness.send(kWmXButtonUp, 2));
    CHECK_FALSE(harness.send(kWmXButtonUp, 2));

    REQUIRE(harness.queued.size() == 2);
    const MouseEvent& down = harness.queued[0];
    const MouseEvent& up = harness.queued[1];
    CHECK(down.button == MouseButton::Mouse5);
    CHECK(down.pressed);
    CHECK(down.modifiers == kCtrl);
    CHECK(up.button == MouseButton::Mouse5);
    CHECK_FALSE(up.pressed);
    CHECK(down.at <= up.at);
    CHECK(harness.asked ==
          std::vector<std::pair<MouseButton, std::uint8_t>>{{MouseButton::Mouse5, kCtrl}});
    CHECK(harness.filter.worst_callback_ms() > 0);
}

// from
// tests/test_personalize_s6_backends.py::test_mouse_hook_leaves_unclaimed_and_injected_buttons_to_the_app
TEST_CASE("test_mouse_hook_leaves_unclaimed_and_injected_buttons_to_the_app",
          "[test_personalize_s6_backends]") {
    HookHarness harness({});
    CHECK_FALSE(harness.send(kWmXButtonDown));
    CHECK_FALSE(harness.send(kWmXButtonUp));
    harness.claimed.insert(MouseButton::Mouse4);
    CHECK_FALSE(harness.send(kWmXButtonDown, 1, kLlmhfInjected));
    CHECK_FALSE(harness.send(kWmXButtonDown, 3));
    CHECK(harness.queued.empty());
    CHECK(harness.asked ==
          std::vector<std::pair<MouseButton, std::uint8_t>>{{MouseButton::Mouse4, 0}});
}

// The fake source keeps the contract the manager relies on: start, a swallowed press delivered to
// on_button, the latency metric, stop. The real source's threads are test_mouse_hook_win.cpp's.
TEST_CASE("the fake mouse hook delivers a swallowed press and reports its latency",
          "[mouse_hook]") {
    std::vector<std::pair<std::string, double>> recorded;
    fv::metrics::set_sink(
        [&recorded](std::string_view name, double value) { recorded.emplace_back(name, value); });
    FakeMouseHook hook;
    std::vector<MouseEvent> delivered;
    hook.start([](MouseButton, std::uint8_t) { return true; },
               [&delivered](const MouseEvent& event) { delivered.push_back(event); });
    CHECK(hook.wait_until_installed(std::chrono::seconds(1)));
    hook.set_now(4.0);
    CHECK(hook.press(MouseButton::Mouse4));
    REQUIRE(delivered.size() == 1);
    CHECK(delivered[0] == MouseEvent{MouseButton::Mouse4, true, 0, 4.0});
    CHECK(hook.running());
    hook.stop();
    CHECK_FALSE(hook.running());
    fv::metrics::set_sink({});
    REQUIRE_FALSE(recorded.empty());
    CHECK(recorded[0] == std::pair<std::string, double>{"mouse_hook_callback_ms", 0.5});
}

// from tests/test_personalize_s6_backends.py::test_mouse_shortcut_support_by_desktop
// The macOS, Wayland and X11 rows are other-platform; FioriVoce binds side buttons only through
// the Windows hook.
TEST_CASE("test_mouse_shortcut_support_by_desktop", "[test_personalize_s6_backends]") {
#ifdef _WIN32
    CHECK(mouse_shortcuts_supported());
#else
    CHECK_FALSE(mouse_shortcuts_supported());
#endif
}

// Side-button holds across a refresh

// from
// tests/test_personalize_fix2_g1_rehook_hold.py::test_windows_side_button_hold_survives_a_rehook
TEST_CASE("test_windows_side_button_hold_survives_a_rehook",
          "[test_personalize_fix2_g1_rehook_hold]") {
    const bool rehook = GENERATE(true, false);
    StampedHarness s({{"record_toggle", "mouse4"}});
    s.button(MouseButton::Mouse4, true, 0, 3.0);
    if (rehook) {
        s.h.manager->rehook();
    } else {
        s.h.manager->update_hotkeys({{"cancel", "kp -"}});
    }
    s.button(MouseButton::Mouse4, false, 0, 9.0);

    CHECK(s.events == Events{{"press", 3.0}, {"release", 9.0}});
    s.h.manager->with_matcher([](HotkeyMatcher& m) { CHECK_FALSE(m.record_key_held()); });
}

// from
// tests/test_personalize_fix2_g1_rehook_hold.py::test_windows_side_button_command_hold_survives_a_rehook
TEST_CASE("test_windows_side_button_command_hold_survives_a_rehook",
          "[test_personalize_fix2_g1_rehook_hold]") {
    StampedHarness s({{"command_mode", "ctrl+mouse5"}});
    s.button(MouseButton::Mouse5, true, kCtrl, 3.0);
    s.h.manager->rehook();
    s.button(MouseButton::Mouse5, false, 0, 9.0);

    CHECK(s.events == Events{{"command_press", 3.0}, {"command_release", 9.0}});
}

// from
// tests/test_personalize_fix2_g1_rehook_hold.py::test_capture_suspension_still_forgets_side_button_holds
TEST_CASE("test_capture_suspension_still_forgets_side_button_holds",
          "[test_personalize_fix2_g1_rehook_hold]") {
    StampedHarness s({{"record_toggle", "mouse4"}});
    s.button(MouseButton::Mouse4, true, 0, 3.0);
    s.h.manager->set_capture_suspended(true);
    s.h.manager->set_capture_suspended(false);

    s.h.manager->with_matcher([](HotkeyMatcher& m) { CHECK_FALSE(m.record_key_held()); });
}

// from
// tests/test_hook_refresh_waits_for_hold.py::test_a_side_button_hold_does_not_hold_back_the_refresh
TEST_CASE("test_a_side_button_hold_does_not_hold_back_the_refresh",
          "[test_hook_refresh_waits_for_hold]") {
    // The runtime wires push-and-hold (_wired_windows_manager). Side-button holds survive a
    // rehook, so the refresh need not wait.
    StampedHarness s({{"record_toggle", "mouse4"}});
    const double now = monotonic_clock()();
    s.button(MouseButton::Mouse4, true, 0, now);

    CHECK_FALSE(s.h.manager->keyboard_hold_active());
    CHECK_FALSE(s.h.manager->keyboard_hold_active(now));
}

// The claim rule end to end: the fake hook runs the real decision with the manager's claims.

TEST_CASE("a claimed side button is swallowed, press and release, and drives the shortcut",
          "[mouse_hook]") {
    StampedHarness s({{"record_toggle", "mouse4"}});
    FakeMouseHook* hook = s.h.mouse;
    REQUIRE(hook->running());
    hook->set_now(2.0);
    CHECK(hook->press(MouseButton::Mouse4));
    hook->set_now(2.5);
    CHECK(hook->release(MouseButton::Mouse4));
    // Back and Forward with no shortcut reach the app.
    CHECK_FALSE(hook->press(MouseButton::Mouse5));
    CHECK_FALSE(hook->release(MouseButton::Mouse5));
    // A modifier the shortcut does not have is another shortcut.
    hook->set_pressed({kVkShift});
    CHECK_FALSE(hook->press(MouseButton::Mouse4));
    CHECK_FALSE(hook->release(MouseButton::Mouse4));
    CHECK(s.events == Events{{"press", 2.0}, {"release", 2.5}});
}

TEST_CASE("the release of a press swallowed before a restart is swallowed after it",
          "[mouse_hook]") {
    StampedHarness s({{"record_toggle", "mouse4"}});
    FakeMouseHook* hook = s.h.mouse;
    hook->set_now(1.0);
    CHECK(hook->press(MouseButton::Mouse4));
    s.h.manager->rehook();
    REQUIRE(hook->starts() == 2);
    hook->set_now(1.5);
    CHECK(hook->release(MouseButton::Mouse4));
    CHECK(s.events == Events{{"press", 1.0}, {"release", 1.5}});
    // Once released, another release is the app's.
    CHECK_FALSE(hook->release(MouseButton::Mouse4));
}

TEST_CASE("while disabled only a side-button enable/disable is claimed", "[mouse_hook]") {
    Harness h(windows_hotkeys({{"enable_disable", "mouse5"}, {"record_toggle", "mouse4"}}));
    std::vector<std::string> statuses;
    h.manager->set_callbacks({.on_status_update_auto_hide = [&statuses](const std::string& s) {
        statuses.push_back(s);
    }});
    FakeMouseHook* hook = h.attach_mouse();
    h.manager->set_program_enabled(false);
    CHECK_FALSE(hook->press(MouseButton::Mouse4));
    CHECK_FALSE(hook->release(MouseButton::Mouse4));
    CHECK(hook->press(MouseButton::Mouse5));
    CHECK(hook->release(MouseButton::Mouse5));
    CHECK(statuses == std::vector<std::string>{"STT Enabled"});
    CHECK(h.manager->program_enabled());
}

TEST_CASE("a side-button decision that throws lets the click through", "[mouse_hook]") {
    HookHarness harness;
    harness.raise = true;
    harness.pressed = {kVkControl};
    CHECK_FALSE(harness.send(kWmXButtonDown));
    CHECK_FALSE(harness.send(kWmXButtonUp));
    CHECK(harness.queued.empty());
    CHECK(harness.asked.size() == 1);
    CHECK_FALSE(harness.filter.suppressed(MouseButton::Mouse4));
}

TEST_CASE("a negative hook code passes untouched but is timed", "[mouse_hook]") {
    HookHarness harness;
    CHECK_FALSE(harness.send(kWmXButtonDown, 1, 0, -1));
    CHECK(harness.asked.empty());
    CHECK(harness.filter.worst_callback_ms() > 0);
}

TEST_CASE("the manager stops the side-button hook it replaces and when it is destroyed",
          "[mouse_hook]") {
    // Counts outside the source, which the manager destroys.
    struct CountingSource final : MouseHookSource {
        explicit CountingSource(int& stops) : stops(stops) {}
        void start(Claims, OnButton) override { started = true; }
        void stop() override {
            stops += started ? 1 : 0;
            started = false;
        }
        bool running() const override { return started; }
        bool wait_until_installed(std::chrono::milliseconds) override { return started; }
        int& stops;
        bool started = false;
    };
    int first_stops = 0;
    int second_stops = 0;
    auto h = std::make_unique<Harness>(windows_hotkeys({{"cancel", "mouse5"}}));
    h->manager->set_mouse_hook_source(std::make_unique<CountingSource>(first_stops));
    CHECK(h->manager->mouse_hook_running());
    h->manager->set_mouse_hook_source(std::make_unique<CountingSource>(second_stops));
    CHECK(first_stops == 1);
    CHECK(h->manager->mouse_hook_running());
    h->manager.reset();
    CHECK(second_stops == 1);
}

// HotkeyManager.__init__ builds its MouseButtonHook and syncs it, so a side-button shortcut
// already in the settings is hooked from the start, with no extra call by the composition.
TEST_CASE("a manager built with a side-button hook starts it for a saved side-button shortcut",
          "[mouse_hook]") {
    auto mouse = std::make_unique<FakeMouseHook>();
    FakeMouseHook* hook = mouse.get();
    HotkeyManager manager(windows_hotkeys({{"record_toggle", "mouse4"}}),
                          std::make_unique<FakeKeyboardHook>(), monotonic_clock(),
                          std::move(mouse));
    manager.set_spawner([](std::function<void()> call) { call(); });
    int toggles = 0;
    Callbacks callbacks;
    callbacks.on_record_toggle = [&toggles] {
        ++toggles;
    };
    manager.set_callbacks(std::move(callbacks));

    CHECK(hook->starts() == 1);
    CHECK(manager.mouse_hook_running());
    CHECK(hook->press(MouseButton::Mouse4));
    CHECK(hook->release(MouseButton::Mouse4));
    CHECK(toggles == 1);
    // Mouse 5 stays the browser's Forward.
    CHECK_FALSE(hook->press(MouseButton::Mouse5));

    manager.update_hotkeys({{"record_toggle", "kp *"}});
    CHECK_FALSE(hook->running());
    CHECK(hook->stops() == 1);
}

TEST_CASE("a manager built with a side-button hook leaves it off until a shortcut wants it",
          "[mouse_hook]") {
    auto mouse = std::make_unique<FakeMouseHook>();
    FakeMouseHook* hook = mouse.get();
    HotkeyManager manager(windows_hotkeys(), std::make_unique<FakeKeyboardHook>(),
                          monotonic_clock(), std::move(mouse));
    CHECK(hook->starts() == 0);
    CHECK_FALSE(manager.mouse_hook_running());
    manager.set_dynamic_hotkeys("transform", {{"polish", "mouse5"}}, [](const std::string&) {});
    CHECK(hook->starts() == 1);
    manager.set_capture_suspended(true);
    CHECK_FALSE(manager.mouse_hook_running());
}

TEST_CASE("a manager on an injected keyboard hook hooks no side button by itself", "[mouse_hook]") {
    // Tests on a fake keyboard hook must never get the real mouse hook.
    HotkeyManager manager(windows_hotkeys({{"record_toggle", "mouse4"}}),
                          std::make_unique<FakeKeyboardHook>());
    CHECK_FALSE(manager.mouse_hook_running());
}

TEST_CASE("a side-button hook that cannot start leaves keyboard shortcuts alone", "[mouse_hook]") {
    Harness h(windows_hotkeys());
    FakeMouseHook* hook = h.attach_mouse();
    hook->fail_next_start("Windows refused the mouse hook.");
    h.manager->update_hotkeys({{"cancel", "mouse4"}});
    CHECK_FALSE(hook->running());
    CHECK(h.manager->backend_available());
    CHECK_FALSE(h.passes(h.down("*", 55, true)));
}

TEST_CASE("the side-button decision does not allocate inside the hook", "[mouse_hook]") {
    Harness h(windows_hotkeys({{"record_toggle", "ctrl+mouse4"}}));
    h.manager->set_dynamic_hotkeys("transform", {{"polish", "alt+mouse5"}},
                                   [](const std::string&) {});
    MouseButtonFilter filter;
    std::array<MouseEvent, 8> queued{};
    std::size_t count = 0;
    // Built once, as the hook's run builds them; calling them allocates nothing.
    const MouseButtonFilter::KeyState key_state = [](int vk) -> short {
        return vk == kVkControl ? short{-32768} : short{0};
    };
    const MouseHookSource::Claims claims = [&h](MouseButton button, std::uint8_t modifiers) {
        return h.manager->claims_mouse_button(button, modifiers);
    };
    const MonotonicClock clock = [] {
        return 1.0;
    };
    const MouseButtonFilter::Dispatch dispatch = [&queued, &count](const MouseEvent& event) {
        queued[count++ % queued.size()] = event;
    };
    std::array<bool, 6> swallowed{};
    {
        fv::testkit::RealtimeGuard guard;
        swallowed[0] =
            filter.decide(0, kWmXButtonDown, 1U << 16, 0, key_state, claims, clock, dispatch);
        swallowed[1] =
            filter.decide(0, kWmXButtonUp, 1U << 16, 0, key_state, claims, clock, dispatch);
        swallowed[2] =
            filter.decide(0, kWmXButtonDown, 2U << 16, 0, key_state, claims, clock, dispatch);
        swallowed[3] =
            filter.decide(0, kWmXButtonUp, 2U << 16, 0, key_state, claims, clock, dispatch);
        swallowed[4] = filter.decide(0, 0x0200, 0, 0, key_state, claims, clock, dispatch);
        swallowed[5] = filter.decide(0, kWmXButtonDown, 1U << 16, kLlmhfInjected, key_state, claims,
                                     clock, dispatch);
    }
    CHECK(swallowed == std::array<bool, 6>{true, true, false, false, false, false});
    CHECK(count == 2);
}

TEST_CASE("side-button names and X buttons", "[mouse_hook]") {
    CHECK(mouse_button_name(MouseButton::Mouse4) == "mouse4");
    CHECK(mouse_button_name(MouseButton::Mouse5) == "mouse5");
    CHECK(mouse_button_from_name("mouse5") == MouseButton::Mouse5);
    CHECK_FALSE(mouse_button_from_name("Mouse5").has_value());
    CHECK(mouse_button_from_xbutton(1) == MouseButton::Mouse4);
    CHECK(mouse_button_from_xbutton(2) == MouseButton::Mouse5);
    CHECK_FALSE(mouse_button_from_xbutton(0).has_value());
    CHECK_FALSE(mouse_button_from_xbutton(3).has_value());
    CHECK(held_modifiers(
              [](int vk) -> short { return vk == kVkRWin ? short{-32768} : short{1}; }) == kWin);
}
