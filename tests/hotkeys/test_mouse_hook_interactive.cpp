// Tests of the real WH_MOUSE_LL hook and the real capture hook. They install global hooks, so
// they carry [interactive]: every default preset leaves them out, and only
// `windows-interactive` runs them, with the owner's go-ahead. Windows ignores injected side
// buttons by design (LLMHF_INJECTED), so the side-button tests ask the person at the machine to
// click: they wait for a real Mouse 4 click and fail when none comes.
#ifdef _WIN32

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/testing/manual_executor.h"
#include "hotkeys/capture.h"
#include "hotkeys/hotkey_manager.h"
#include "hotkeys/mouse_hook.h"

using namespace fv::hotkeys;
using namespace std::chrono_literals;

namespace {

// Stamped record and Command Mode callbacks, waited for from the test thread.
class Recorder {
public:
    void add(std::string name) {
        {
            std::lock_guard lock(mutex_);
            events_.push_back(std::move(name));
        }
        changed_.notify_all();
    }
    bool wait_for(std::size_t count, std::chrono::seconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return events_.size() >= count; });
    }
    std::vector<std::string> events() {
        std::lock_guard lock(mutex_);
        return events_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> events_;
};

void send_keys(std::initializer_list<WORD> keys) {
    std::vector<INPUT> inputs;
    for (const WORD vk : keys) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = vk;
        inputs.push_back(input);
    }
    for (auto it = keys.end(); it != keys.begin();) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = *--it;
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(input);
    }
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

} // namespace

// The real hook threads: installed, named, removed by stop() on their own thread.
TEST_CASE("the real mouse hook installs on its own thread and stop removes it", "[interactive]") {
    auto source = make_mouse_hook_source();
    source->start([](MouseButton, std::uint8_t) { return false; }, [](const MouseEvent&) {});
    REQUIRE(source->wait_until_installed(1s));
    CHECK(source->running());
    // A second start while running keeps the one installation.
    source->start([](MouseButton, std::uint8_t) { return true; }, [](const MouseEvent&) {});
    CHECK(source->running());
    source->stop();
    CHECK_FALSE(source->running());
    CHECK_FALSE(source->wait_until_installed(10ms));
    source->start([](MouseButton, std::uint8_t) { return false; }, [](const MouseEvent&) {});
    CHECK(source->wait_until_installed(1s));
    source->stop();
}

// test_real_mouse_hook_delivers_the_release_after_a_refresh with a real click; the port over a
// fake user32 is test_mouse_hook_win.cpp's.
TEST_CASE("a real Mouse 4 hold delivers its release after a refresh", "[interactive]") {
    HotkeyManager manager;
    REQUIRE(manager.backend_available());
    manager.set_record_mode(TriggerMode::PushHold);
    Recorder recorder;
    Callbacks callbacks;
    callbacks.on_record_press = [&recorder](double) {
        recorder.add("press");
    };
    callbacks.on_record_release = [&recorder](double) {
        recorder.add("release");
    };
    manager.set_callbacks(callbacks);
    manager.update_hotkeys({{"record_toggle", "mouse4"}});
    REQUIRE(manager.mouse_hook_running());

    std::cout << "Press and hold Mouse 4 (Back) for a second, then let go, within 15 seconds."
              << std::endl;
    REQUIRE(recorder.wait_for(1, 15s));
    // The refresh lands while the button is down; the release still reaches the shortcut.
    manager.rehook();
    REQUIRE(recorder.wait_for(2, 15s));
    CHECK(recorder.events() == std::vector<std::string>{"press", "release"});
    manager.cleanup();
    CHECK_FALSE(manager.mouse_hook_running());
}

// The app's manager hooks a saved side-button shortcut by itself (the spec's demo: a "mouse4"
// record shortcut toggles dictation and the browser does not go Back), with no
// set_mouse_hook_source() from the composition.
TEST_CASE("the app's manager hooks a saved side-button shortcut from the start", "[interactive]") {
    HotkeyManager manager({{"record_toggle", "mouse4"}});
    REQUIRE(manager.backend_available());
    REQUIRE(manager.mouse_hook_running());
    Recorder recorder;
    Callbacks callbacks;
    callbacks.on_record_toggle = [&recorder] {
        recorder.add("toggle");
    };
    manager.set_callbacks(callbacks);

    std::cout << "Click Mouse 4 (Back) once in a browser within 15 seconds; the page must not go "
                 "back."
              << std::endl;
    REQUIRE(recorder.wait_for(1, 15s));
    CHECK(recorder.events() == std::vector<std::string>{"toggle"});
    manager.cleanup();
    CHECK_FALSE(manager.mouse_hook_running());
}

TEST_CASE("the real capture hook sees keys and leaves them to the app", "[interactive]") {
    auto capture = make_hotkey_capture();
    fv::ManualExecutor ui;
    std::vector<std::string> captured;
    auto subscription = capture->captured.connect(
        ui, [&captured](const std::string& hotkey) { captured.push_back(hotkey); });
    capture->start();
    std::this_thread::sleep_for(300ms);
    send_keys({VK_CONTROL, VK_F9});
    REQUIRE(capture->wait(2000ms));
    ui.run_all();
    CHECK(captured == std::vector<std::string>{"ctrl+f9"});
}

TEST_CASE("the real capture hook treats Escape as a cancel", "[interactive]") {
    auto capture = make_hotkey_capture();
    fv::ManualExecutor ui;
    int cancelled = 0;
    auto subscription = capture->cancelled.connect(ui, [&cancelled] { ++cancelled; });
    capture->start();
    std::this_thread::sleep_for(300ms);
    send_keys({VK_ESCAPE});
    REQUIRE(capture->wait(2000ms));
    ui.run_all();
    CHECK(cancelled == 1);
}

#endif
