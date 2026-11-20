// The Windows side-button hook's own threads (win/mouse_hook.cpp) over a fake user32, as
// OpenWhisper's tests drive MouseButtonHook with _FakeUser32: no global hook is installed and no
// input is injected, so these run unattended. The real-hook variants are in
// test_mouse_hook_interactive.cpp.
#ifdef _WIN32

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/metrics.h"
#include "harness.h"
#include "hotkeys/win/mouse_hook_win32.h"

using namespace fv::hotkeys;
using namespace std::chrono_literals;
using fv::hotkeys::testing::Harness;
using fv::hotkeys::testing::windows_hotkeys;

namespace {

const HHOOK kHandle = reinterpret_cast<HHOOK>(static_cast<std::uintptr_t>(0xBEEF));
constexpr DWORD kThreadId = 77;
constexpr std::uint8_t kCtrlWin = kCtrl | kWin;

// _FakeUser32 and _FakeKernel32. GetMessageW runs the hook procedure for each call() on the
// thread that installed it, as Windows does, and returns 0 once WM_QUIT is posted (a fresh loop
// per installation, as test_personalize_fix2_g1_rehook_hold's _User32).
class FakeUser32 {
public:
    MouseHookWin32 table() {
        MouseHookWin32 win32;
        win32.set_hook = [this](HOOKPROC proc) -> HHOOK {
            std::lock_guard lock(mutex_);
            ++installs_;
            if (refuse_) {
                return nullptr;
            }
            proc_ = proc;
            changed_.notify_all();
            return kHandle;
        };
        win32.unhook = [this](HHOOK hook) -> BOOL {
            std::lock_guard lock(mutex_);
            unhooked_.push_back(hook);
            // An unhooked procedure is called no more; call() waits for the next installation.
            proc_ = nullptr;
            return TRUE;
        };
        win32.get_message = [this](MSG*) -> BOOL {
            std::unique_lock lock(mutex_);
            for (;;) {
                changed_.wait(lock, [this] { return quit_ || !calls_.empty(); });
                if (!calls_.empty()) {
                    Call* call = calls_.front();
                    calls_.pop_front();
                    const HOOKPROC proc = proc_;
                    lock.unlock();
                    const LRESULT result =
                        proc(call->code, call->message, reinterpret_cast<LPARAM>(&call->info));
                    lock.lock();
                    call->result = result;
                    changed_.notify_all();
                    continue;
                }
                quit_ = false;
                return 0;
            }
        };
        win32.post_thread_message = [this](DWORD thread_id, UINT message) -> BOOL {
            std::lock_guard lock(mutex_);
            posted_.emplace_back(thread_id, message);
            if (message == WM_QUIT) {
                quit_ = true;
            }
            changed_.notify_all();
            return TRUE;
        };
        win32.call_next = [this](int, WPARAM message, LPARAM) -> LRESULT {
            std::lock_guard lock(mutex_);
            next_calls_.push_back(static_cast<UINT>(message));
            return 0;
        };
        win32.get_async_key_state = [this](int vk) -> SHORT {
            std::lock_guard lock(mutex_);
            return pressed_.contains(vk) ? SHORT{-32768} : SHORT{0};
        };
        win32.current_thread_id = [] {
            return kThreadId;
        };
        return win32;
    }

    // One hook call with a mouse message, made on the hook thread; what the procedure returned,
    // or nullopt when no installed hook took it within 2 s.
    std::optional<LRESULT> call(UINT message, unsigned xbutton = 1, DWORD flags = 0) {
        Call call;
        call.message = message;
        call.info.mouseData = static_cast<DWORD>(xbutton) << 16;
        call.info.flags = flags;
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 2s, [this] { return proc_ != nullptr; })) {
            return std::nullopt;
        }
        calls_.push_back(&call);
        changed_.notify_all();
        if (!changed_.wait_for(lock, 2s, [&call] { return call.result.has_value(); })) {
            std::erase(calls_, &call);
            return std::nullopt;
        }
        return call.result;
    }

    void refuse() {
        std::lock_guard lock(mutex_);
        refuse_ = true;
    }
    void set_pressed(std::set<int> keys) {
        std::lock_guard lock(mutex_);
        pressed_ = std::move(keys);
    }
    std::vector<std::pair<DWORD, UINT>> posted() {
        std::lock_guard lock(mutex_);
        return posted_;
    }
    std::vector<HHOOK> unhooked() {
        std::lock_guard lock(mutex_);
        return unhooked_;
    }
    std::vector<UINT> next_calls() {
        std::lock_guard lock(mutex_);
        return next_calls_;
    }
    int installs() {
        std::lock_guard lock(mutex_);
        return installs_;
    }

private:
    struct Call {
        int code = 0;
        UINT message = 0;
        MSLLHOOKSTRUCT info{};
        std::optional<LRESULT> result;
    };

    std::mutex mutex_;
    std::condition_variable changed_;
    HOOKPROC proc_ = nullptr;
    bool refuse_ = false;
    bool quit_ = false;
    int installs_ = 0;
    std::deque<Call*> calls_;
    std::set<int> pressed_;
    std::vector<std::pair<DWORD, UINT>> posted_;
    std::vector<HHOOK> unhooked_;
    std::vector<UINT> next_calls_;
};

// What the dispatch thread delivered, waited for from the test thread.
template <class T> class Delivered {
public:
    // Notifies under the lock, so a waiter that sees the item cannot destroy this while the
    // notification is still being sent.
    void add(T item) {
        std::lock_guard lock(mutex_);
        items_.push_back(std::move(item));
        changed_.notify_all();
    }
    bool wait_for(std::size_t count) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 2s, [&] { return items_.size() >= count; });
    }
    std::vector<T> items() {
        std::lock_guard lock(mutex_);
        return items_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<T> items_;
};

// Clears the metrics sink however the test ends.
struct MetricsSink {
    explicit MetricsSink(std::function<void(std::string_view, double)> sink) {
        fv::metrics::set_sink(std::move(sink));
    }
    ~MetricsSink() { fv::metrics::set_sink({}); }
};

} // namespace

// from
// tests/test_personalize_s6_backends.py::test_mouse_hook_thread_installs_dispatches_and_unhooks
TEST_CASE("test_mouse_hook_thread_installs_dispatches_and_unhooks",
          "[test_personalize_s6_backends]") {
    Delivered<std::pair<std::string, double>> recorded;
    MetricsSink sink([&recorded](std::string_view name, double value) {
        recorded.add({std::string(name), value});
    });
    FakeUser32 user32;
    Delivered<MouseEvent> delivered;
    auto hook = make_mouse_hook_source([] { return 4.0; }, user32.table());
    hook->start(
        [](MouseButton, std::uint8_t) {
            // Slow enough that the callback's latency is never 0 on a coarse clock.
            std::this_thread::sleep_for(1ms);
            return true;
        },
        [&delivered](const MouseEvent& event) { delivered.add(event); });
    CHECK(hook->wait_until_installed(2s));
    CHECK(user32.call(WM_XBUTTONDOWN) == LRESULT{1});
    REQUIRE(delivered.wait_for(1));
    CHECK(delivered.items()[0] == MouseEvent{MouseButton::Mouse4, true, 0, 4.0});
    CHECK(hook->running());
    REQUIRE(recorded.wait_for(1));
    hook->stop();

    CHECK(user32.posted() == std::vector<std::pair<DWORD, UINT>>{{kThreadId, WM_QUIT}});
    CHECK(user32.unhooked() == std::vector<HHOOK>{kHandle});
    CHECK_FALSE(hook->running());
    CHECK(recorded.items()[0].first == "mouse_hook_callback_ms");
    CHECK(recorded.items()[0].second > 0);
}

TEST_CASE("the side-button hook passes other mouse messages to the next hook", "[mouse_hook]") {
    FakeUser32 user32;
    Delivered<MouseEvent> delivered;
    auto hook = make_mouse_hook_source([] { return 1.0; }, user32.table());
    hook->start([](MouseButton, std::uint8_t) { return true; },
                [&delivered](const MouseEvent& event) { delivered.add(event); });
    REQUIRE(hook->wait_until_installed(2s));
    for (const UINT message : {0x0200U, 0x0201U, 0x0202U, 0x020AU}) {
        CHECK(user32.call(message) == LRESULT{0});
    }
    // An injected side button and an unknown X button are the app's too.
    CHECK(user32.call(WM_XBUTTONDOWN, 1, LLMHF_INJECTED) == LRESULT{0});
    CHECK(user32.call(WM_XBUTTONDOWN, 3) == LRESULT{0});
    hook->stop();
    CHECK(user32.next_calls() ==
          std::vector<UINT>{0x0200, 0x0201, 0x0202, 0x020A, WM_XBUTTONDOWN, WM_XBUTTONDOWN});
    CHECK(delivered.items().empty());
}

TEST_CASE("the side-button hook reads the modifiers held at the press", "[mouse_hook]") {
    FakeUser32 user32;
    Delivered<MouseEvent> delivered;
    std::vector<std::pair<MouseButton, std::uint8_t>> asked;
    auto hook = make_mouse_hook_source([] { return 2.0; }, user32.table());
    hook->start(
        [&asked](MouseButton button, std::uint8_t modifiers) {
            asked.emplace_back(button, modifiers);
            return true;
        },
        [&delivered](const MouseEvent& event) { delivered.add(event); });
    REQUIRE(hook->wait_until_installed(2s));
    user32.set_pressed({VK_CONTROL, VK_RWIN});
    CHECK(user32.call(WM_XBUTTONDOWN, 2) == LRESULT{1});
    user32.set_pressed({});
    CHECK(user32.call(WM_XBUTTONUP, 2) == LRESULT{1});
    REQUIRE(delivered.wait_for(2));
    hook->stop();
    CHECK(asked ==
          std::vector<std::pair<MouseButton, std::uint8_t>>{{MouseButton::Mouse5, kCtrlWin}});
    CHECK(delivered.items() == std::vector<MouseEvent>{{MouseButton::Mouse5, true, kCtrlWin, 2.0},
                                                       {MouseButton::Mouse5, false, 0, 2.0}});
}

TEST_CASE("a refused side-button hook reports it and stops cleanly", "[mouse_hook]") {
    FakeUser32 user32;
    user32.refuse();
    auto hook = make_mouse_hook_source([] { return 0.0; }, user32.table());
    hook->start([](MouseButton, std::uint8_t) { return true; }, [](const MouseEvent&) {});
    CHECK_FALSE(hook->wait_until_installed(2s));
    hook->stop();
    CHECK(user32.installs() == 1);
    CHECK(user32.unhooked().empty());
    CHECK_FALSE(hook->running());
    // A new start installs again.
    hook->start([](MouseButton, std::uint8_t) { return true; }, [](const MouseEvent&) {});
    CHECK_FALSE(hook->wait_until_installed(2s));
    CHECK(user32.installs() == 2);
    hook->stop();
}

TEST_CASE("a second start keeps the one installation", "[mouse_hook]") {
    FakeUser32 user32;
    auto hook = make_mouse_hook_source([] { return 0.0; }, user32.table());
    hook->start([](MouseButton, std::uint8_t) { return false; }, [](const MouseEvent&) {});
    REQUIRE(hook->wait_until_installed(2s));
    hook->start([](MouseButton, std::uint8_t) { return true; }, [](const MouseEvent&) {});
    // The first start's claims still decide.
    CHECK(user32.call(WM_XBUTTONDOWN) == LRESULT{0});
    hook->stop();
    hook->stop();
    CHECK(user32.installs() == 1);
    CHECK(user32.unhooked() == std::vector<HHOOK>{kHandle});
}

TEST_CASE("stop from inside on_button does not wait for itself", "[mouse_hook]") {
    FakeUser32 user32;
    auto hook = make_mouse_hook_source([] { return 0.0; }, user32.table());
    MouseHookSource* raw = hook.get();
    Delivered<bool> stopped;
    hook->start([](MouseButton, std::uint8_t) { return true; },
                [raw, &stopped](const MouseEvent&) {
                    raw->stop();
                    stopped.add(true);
                });
    REQUIRE(hook->wait_until_installed(2s));
    CHECK(user32.call(WM_XBUTTONDOWN) == LRESULT{1});
    REQUIRE(stopped.wait_for(1));
    CHECK_FALSE(hook->running());
    CHECK(user32.unhooked() == std::vector<HHOOK>{kHandle});
}

// from
// tests/test_personalize_fix2_g1_rehook_hold.py::test_real_mouse_hook_delivers_the_release_after_a_refresh
// The hook and dispatch threads are the real ones; user32 is the fake, as in OpenWhisper. The
// variant with a real click is test_mouse_hook_interactive.cpp's.
TEST_CASE("test_real_mouse_hook_delivers_the_release_after_a_refresh",
          "[test_personalize_fix2_g1_rehook_hold]") {
    FakeUser32 user32;
    Delivered<std::string> calls;
    Harness h(windows_hotkeys({{"record_toggle", "mouse4"}}));
    auto source = make_mouse_hook_source([] { return 5.0; }, user32.table());
    // The manager owns the source until the end of the test.
    MouseHookSource* hook = source.get();
    h.manager->set_mouse_hook_source(std::move(source));
    h.manager->set_record_mode(TriggerMode::PushHold);
    Callbacks callbacks;
    callbacks.on_record_press = [&calls](double) {
        calls.add("start");
    };
    callbacks.on_record_release = [&calls](double) {
        calls.add("stop");
    };
    h.manager->set_callbacks(std::move(callbacks));
    REQUIRE(h.manager->mouse_hook_running());

    CHECK(user32.call(WM_XBUTTONDOWN) == LRESULT{1});
    REQUIRE(calls.wait_for(1));
    h.manager->rehook();
    // start() returns before the new hook thread installs, as Python's does.
    REQUIRE(hook->wait_until_installed(2s));
    CHECK(user32.installs() == 2);
    CHECK(user32.call(WM_XBUTTONUP) == LRESULT{1});
    REQUIRE(calls.wait_for(2));
    h.manager->cleanup();

    CHECK(calls.items() == std::vector<std::string>{"start", "stop"});
    CHECK(user32.unhooked() == std::vector<HHOOK>{kHandle, kHandle});
    CHECK_FALSE(h.manager->mouse_hook_running());
}

TEST_CASE("a stop waits only briefly for a callback that waits on the stopping thread",
          "[mouse_hook]") {
    FakeUser32 user32;
    std::promise<void> stop_returned;
    const std::shared_future<void> returned = stop_returned.get_future().share();
    Delivered<std::string> steps;
    auto hook = make_mouse_hook_source([] { return 3.0; }, user32.table());
    hook->start([](MouseButton, std::uint8_t) { return true; },
                [&steps, returned](const MouseEvent& event) {
                    steps.add(event.pressed ? "entered" : "release");
                    if (event.pressed) {
                        returned.wait();
                        steps.add("finished");
                    }
                });
    REQUIRE(hook->wait_until_installed(2s));
    CHECK(user32.call(WM_XBUTTONDOWN) == LRESULT{1});
    REQUIRE(steps.wait_for(1));

    const auto began = std::chrono::steady_clock::now();
    hook->stop();
    const auto took = std::chrono::steady_clock::now() - began;
    // The hook thread ends at once; the dispatch thread gets 1 s, then is left to finish.
    CHECK(took < 3s);
    CHECK_FALSE(hook->running());
    CHECK(user32.unhooked() == std::vector<HHOOK>{kHandle});
    CHECK(steps.items() == std::vector<std::string>{"entered"});

    stop_returned.set_value();
    REQUIRE(steps.wait_for(2));
    // The source can start again while nothing is left over from the earlier run.
    hook->start([](MouseButton, std::uint8_t) { return true; },
                [&steps](const MouseEvent&) { steps.add("second run"); });
    REQUIRE(hook->wait_until_installed(2s));
    CHECK(user32.call(WM_XBUTTONDOWN) == LRESULT{1});
    REQUIRE(steps.wait_for(3));
    hook.reset();
    CHECK(steps.items() == std::vector<std::string>{"entered", "finished", "second run"});
}

TEST_CASE("destroying the source waits for a callback a stop left running", "[mouse_hook]") {
    FakeUser32 user32;
    std::atomic<bool> go_on{false};
    std::atomic<bool> finished{false};
    Delivered<bool> entered;
    auto hook = make_mouse_hook_source([] { return 3.0; }, user32.table());
    hook->start([](MouseButton, std::uint8_t) { return true; },
                [&](const MouseEvent&) {
                    entered.add(true);
                    while (!go_on.load()) {
                        std::this_thread::sleep_for(1ms);
                    }
                    // Still in the owner's callback well after the release.
                    std::this_thread::sleep_for(50ms);
                    finished = true;
                });
    REQUIRE(hook->wait_until_installed(2s));
    CHECK(user32.call(WM_XBUTTONDOWN) == LRESULT{1});
    REQUIRE(entered.wait_for(1));
    hook->stop();
    CHECK_FALSE(finished.load());

    std::thread releaser([&go_on] {
        std::this_thread::sleep_for(100ms);
        go_on = true;
    });
    hook.reset();
    CHECK(finished.load());
    releaser.join();
}

TEST_CASE("destroying the source delivers nothing queued behind a callback a stop left running",
          "[mouse_hook]") {
    FakeUser32 user32;
    std::atomic<bool> go_on{false};
    Delivered<std::string> steps;
    auto hook = make_mouse_hook_source([] { return 3.0; }, user32.table());
    hook->start([](MouseButton, std::uint8_t) { return true; },
                [&](const MouseEvent& event) {
                    steps.add(event.pressed ? "press" : "release");
                    while (!go_on.load()) {
                        std::this_thread::sleep_for(1ms);
                    }
                });
    REQUIRE(hook->wait_until_installed(2s));
    CHECK(user32.call(WM_XBUTTONDOWN) == LRESULT{1});
    REQUIRE(steps.wait_for(1));
    // The release of the swallowed press is swallowed too, and queued behind its callback.
    CHECK(user32.call(WM_XBUTTONUP) == LRESULT{1});
    hook->stop();

    std::thread releaser([&go_on] {
        std::this_thread::sleep_for(100ms);
        go_on = true;
    });
    hook.reset();
    releaser.join();
    CHECK(steps.items() == std::vector<std::string>{"press"});
}

// The deadlock a joined dispatch thread allowed: a side-button callback hands its event to the
// GUI thread and waits for it, while the GUI thread is changing the shortcuts (which stops the
// hook). The change now returns, and the callback finishes once the GUI thread is free.
TEST_CASE("changing the shortcuts while a side-button callback waits on that thread returns",
          "[mouse_hook]") {
    FakeUser32 user32;
    Delivered<std::string> calls;
    std::promise<void> gui_free;
    const std::shared_future<void> gui_is_free = gui_free.get_future().share();
    Harness h(windows_hotkeys({{"record_toggle", "mouse4"}}));
    auto source = make_mouse_hook_source([] { return 6.0; }, user32.table());
    MouseHookSource* hook = source.get();
    h.manager->set_mouse_hook_source(std::move(source));
    h.manager->set_record_mode(TriggerMode::PushHold);
    Callbacks callbacks;
    callbacks.on_record_press = [&calls, gui_is_free](double) {
        calls.add("press");
        gui_is_free.wait();
        calls.add("handed on");
    };
    h.manager->set_callbacks(std::move(callbacks));
    REQUIRE(hook->wait_until_installed(2s));
    CHECK(user32.call(WM_XBUTTONDOWN) == LRESULT{1});
    REQUIRE(calls.wait_for(1));

    h.manager->update_hotkeys({{"record_toggle", "kp *"}});
    CHECK_FALSE(h.manager->mouse_hook_running());
    gui_free.set_value();
    REQUIRE(calls.wait_for(2));
    CHECK(calls.items() == std::vector<std::string>{"press", "handed on"});
}

#endif
