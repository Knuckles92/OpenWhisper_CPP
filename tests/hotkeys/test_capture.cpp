#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "base/testing/manual_executor.h"
#include "harness.h"
#include "hotkeys/capture.h"
#include "hotkeys/mouse_shortcuts.h"
#include "hotkeys/testing/fake_capture.h"

using namespace fv::hotkeys;
using namespace std::chrono_literals;
using fv::hotkeys::testing::Harness;
using fv::hotkeys::testing::windows_hotkeys;

namespace {

// A capture over its own fake hook, its signals delivered to `ui` as the settings page's
// QueuedConnection would.
struct CaptureHarness {
    CaptureHarness() {
        auto fake = std::make_unique<FakeKeyboardHook>();
        hook = fake.get();
        capture = make_hotkey_capture(std::move(fake));
        subscriptions.push_back(capture->captured.connect(
            ui, [this](const std::string& hotkey) { captured.push_back(hotkey); }));
        subscriptions.push_back(capture->cancelled.connect(ui, [this] { ++cancelled; }));
        subscriptions.push_back(capture->failed.connect(
            ui, [this](const std::string& message) { failed.push_back(message); }));
        subscriptions.push_back(capture->finished.connect(ui, [this] { ++finished; }));
    }

    // Starts and waits until the capture's hook is in.
    bool start() {
        capture->start();
        const auto until = std::chrono::steady_clock::now() + 2s;
        while (!hook->installed()) {
            if (std::chrono::steady_clock::now() > until) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    // A key reaching the capture's hook; true when it passes on to the app.
    bool send(bool down, std::string_view name, bool keypad = false) {
        KeyEvent event;
        event.down = down;
        event.name = name;
        event.keypad = keypad;
        return hook->send(event);
    }

    // Waits for the capture to end, then delivers its signals.
    bool finish() {
        const bool ended = capture->wait(2000ms);
        ui.run_all();
        return ended;
    }

    fv::ManualExecutor ui;
    FakeKeyboardHook* hook = nullptr;
    std::unique_ptr<HotkeyCapture> capture;
    std::vector<fv::Subscription> subscriptions;
    std::vector<std::string> captured;
    int cancelled = 0;
    std::vector<std::string> failed;
    int finished = 0;
};

} // namespace

// from tests/test_personalize_s6_settings.py::test_escape_key_names_are_treated_as_cancel
TEST_CASE("test_escape_key_names_are_treated_as_cancel", "[test_personalize_s6_settings]") {
    CHECK(is_escape_key("esc"));
    CHECK(is_escape_key("Escape"));
    CHECK_FALSE(is_escape_key("f8"));
    CHECK_FALSE(is_escape_key(""));
    CHECK_FALSE(is_escape_key(std::nullopt));
}

// from tests/test_personalize_s6_settings.py::test_windows_capture_removes_only_its_own_hook
TEST_CASE("test_windows_capture_removes_only_its_own_hook", "[test_personalize_s6_settings]") {
    // The app's own suppressing hook, which a capture must leave in place.
    Harness app(windows_hotkeys());
    {
        CaptureHarness c;
        REQUIRE(c.start());
        // Non-suppressing: a key the capture sees still reaches the app.
        CHECK(c.send(true, "ctrl"));
        c.capture->stop();
        REQUIRE(c.finish());
        CHECK(c.hook->uninstalls() == 1);
        CHECK_FALSE(c.hook->installed());
        CHECK(c.captured.empty());
        CHECK(c.cancelled == 0);
        CHECK(c.failed.empty());
    }
    CHECK(app.hook->installed());
    CHECK(app.hook->uninstalls() == 0);

    CaptureHarness c;
    REQUIRE(c.start());
    CHECK(c.send(true, "windows"));
    CHECK(c.send(true, "f9"));
    CHECK(c.send(false, "f9"));
    REQUIRE(c.finish());
    CHECK(c.captured == std::vector<std::string>{"win+f9"});
    CHECK(c.hook->uninstalls() == 1);
    CHECK(app.hook->installed());
    CHECK(app.hook->uninstalls() == 0);
}

// from tests/test_personalize_s6_settings.py::test_windows_capture_treats_escape_as_cancel
TEST_CASE("test_windows_capture_treats_escape_as_cancel", "[test_personalize_s6_settings]") {
    CaptureHarness c;
    REQUIRE(c.start());
    c.send(true, "esc");
    c.send(false, "esc");
    REQUIRE(c.finish());
    CHECK(c.captured.empty());
    CHECK(c.cancelled == 1);
    CHECK(c.failed.empty());
    CHECK(c.finished == 1);
}

TEST_CASE("a capture names keypad keys kp_ and ends on the first key-up", "[capture]") {
    CaptureHarness c;
    REQUIRE(c.start());
    c.send(true, "ctrl");
    c.send(true, "alt");
    c.send(true, "*", true);
    c.send(false, "*", true);
    c.send(true, "x"); // after the result: not part of it
    REQUIRE(c.finish());
    CHECK(c.captured == std::vector<std::string>{"ctrl+alt+kp *"});
    CHECK(c.finished == 1);
    CHECK_FALSE(c.capture->running());
}

TEST_CASE("a key with no name fails the capture with the Windows message", "[capture]") {
    CaptureHarness c;
    REQUIRE(c.start());
    c.send(true, "ctrl");
    c.send(true, "");
    c.send(false, "");
    REQUIRE(c.finish());
    CHECK(c.captured.empty());
    CHECK(c.failed == std::vector<std::string>{"Could not capture hotkey. Please try again."});
    CHECK(c.hook->uninstalls() == 1);
}

TEST_CASE("a capture whose hook cannot be installed fails", "[capture]") {
    CaptureHarness c;
    c.hook->fail_next_install("Global hotkeys are not supported on this platform.");
    c.capture->start();
    REQUIRE(c.finish());
    CHECK(c.failed == std::vector<std::string>{std::string(kHotkeyCaptureFailureMessage)});
    CHECK(c.hook->uninstalls() == 0);
    CHECK(c.finished == 1);
}

TEST_CASE("a stopped capture ends silently and can start again", "[capture]") {
    CaptureHarness c;
    REQUIRE(c.start());
    c.send(true, "ctrl");
    c.capture->stop();
    REQUIRE(c.finish());
    CHECK(c.captured.empty());
    CHECK(c.cancelled == 0);
    CHECK(c.failed.empty());
    CHECK(c.finished == 1);

    REQUIRE(c.start());
    c.send(true, "f8");
    c.send(false, "f8");
    REQUIRE(c.finish());
    CHECK(c.captured == std::vector<std::string>{"f8"});
    CHECK(c.hook->installs() == 2);
}

TEST_CASE("get_hotkey_name normalizes, dedupes and orders the names", "[capture]") {
    CHECK(get_hotkey_name({"+", "left ctrl", "shift"}) == "ctrl+shift+plus");
    CHECK(get_hotkey_name({"windows", "right alt", "Page_Up", "b", "a"}) ==
          "alt+windows+a+b+page up");
    CHECK(get_hotkey_name({"kp_*"}) == "kp *");
    CHECK(get_hotkey_name({"ctrl", "ctrl"}) == "ctrl");
    CHECK_THROWS_AS(get_hotkey_name({"ctrl", ""}), std::invalid_argument);
}

TEST_CASE("the capture's verdict on the keys so far", "[capture]") {
    using Kind = CaptureOutcome::Kind;
    CHECK(capture_outcome({}).kind == Kind::Pending);
    CHECK(capture_outcome({{"ctrl", false, true}}).kind == Kind::Pending);
    CHECK(capture_outcome({{"esc", false, true}, {"esc", false, false}}) ==
          CaptureOutcome{Kind::Cancelled, ""});
    CHECK(capture_outcome({{"ctrl", false, true}, {"esc", false, true}, {"esc", false, false}}) ==
          CaptureOutcome{Kind::Captured, "ctrl+esc"});
    CHECK(capture_outcome({{std::nullopt, true, true}, {std::nullopt, true, false}}) ==
          CaptureOutcome{Kind::Captured, "kp none"});
    CHECK_THROWS_AS(capture_outcome({{std::nullopt, false, false}}), std::invalid_argument);
}

TEST_CASE("the fake capture ends a started capture once, and a stopped one silently", "[capture]") {
    FakeCaptureFactory factory;
    auto capture = factory.factory()();
    REQUIRE(factory.last() == capture.get());
    fv::ManualExecutor ui;
    std::vector<std::string> captured;
    int finished = 0;
    auto s1 = capture->captured.connect(
        ui, [&captured](const std::string& hotkey) { captured.push_back(hotkey); });
    auto s2 = capture->finished.connect(ui, [&finished] { ++finished; });

    FakeCapture* fake = factory.last();
    fake->start();
    CHECK(fake->running());
    fake->capture("ctrl+alt+k");
    fake->capture("ctrl+alt+j");
    ui.run_all();
    CHECK(captured == std::vector<std::string>{"ctrl+alt+k"});
    CHECK(finished == 1);

    fake->start();
    fake->stop();
    fake->cancel();
    ui.run_all();
    CHECK(fake->stopped());
    CHECK(fake->starts() == 2);
    CHECK(finished == 1);
}

TEST_CASE("the fake capture delivers a stopped capture's finished only when the test asks",
          "[capture]") {
    FakeCapture fake;
    fv::ManualExecutor ui;
    int finished = 0;
    int cancelled = 0;
    auto s1 = fake.finished.connect(ui, [&finished] { ++finished; });
    auto s2 = fake.cancelled.connect(ui, [&cancelled] { ++cancelled; });

    // Not started: a stop leaves nothing to finish.
    fake.stop();
    CHECK_FALSE(fake.finish_stopped());

    // The real capture's thread emits finished after stop(), later and on its own thread.
    fake.start();
    fake.stop();
    ui.run_all();
    CHECK(finished == 0);
    CHECK(fake.finish_stopped());
    CHECK_FALSE(fake.finish_stopped());
    ui.run_all();
    CHECK(finished == 1);
    CHECK(cancelled == 0);

    // A capture that already ended has no finished left for a later stop.
    fake.start();
    fake.cancel();
    fake.stop();
    CHECK_FALSE(fake.finish_stopped());
    ui.run_all();
    CHECK(finished == 2);
    CHECK(cancelled == 1);

    // A new start forgets a finished the test never delivered.
    fake.start();
    fake.stop();
    fake.start();
    CHECK_FALSE(fake.finish_stopped());
}

TEST_CASE("side-button notes by backend", "[capture]") {
    CHECK(mouse_shortcut_note("mouse5", Vocabulary::Windows) ==
          "Mouse 5 won't work as Forward in other apps while it's a shortcut.");
    CHECK(mouse_shortcut_note("ctrl+r", Vocabulary::Windows).empty());
    CHECK(mouse_shortcut_note("mouse4", Vocabulary::Pynput) ==
          "Mouse 4 also still goes Back in the app you're using.");
    CHECK(mouse_shortcut_note("alt+mouse4", Vocabulary::Pynput).empty());
#ifdef _WIN32
    CHECK(mouse_shortcut_note("ctrl+mouse4") ==
          "Mouse 4 won't work as Back in other apps while it's a shortcut.");
#endif
}
