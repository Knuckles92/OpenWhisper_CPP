// Passive click observation (services/_mouse_hook_win.py observe_click): the walkthrough's Auto
// Mode sees the release point of each real click that began over another app's window, and the
// app under the pointer always gets the click. The decision runs over FakeMouseHook and, on
// Windows, through the real hook and dispatch threads over a fake user32; the real WH_MOUSE_LL
// variant is [interactive].
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "hotkeys/mouse_hook.h"
#include "hotkeys/testing/fake_mouse_hook.h"

using namespace fv::hotkeys;
using namespace std::chrono_literals;

namespace {

// What the observer was handed, in order.
struct Observed {
    std::vector<Click> clicks;
    ClickObserver observer() {
        return [this](int x, int y, double at) {
            clicks.push_back(Click{x, y, at});
        };
    }
};

constexpr unsigned kWmMButtonDown = 0x0207;
constexpr unsigned kWmMButtonUp = 0x0208;
constexpr unsigned kWmMouseMove = 0x0200;

} // namespace

// from
// tests/test_personalize_s6_backends.py::test_mouse_hook_observes_only_external_real_clicks_and_passes_them_through
TEST_CASE("test_mouse_hook_observes_only_external_real_clicks_and_passes_them_through",
          "[test_personalize_s6_backends]") {
    Observed observed;
    FakeMouseHook hook(observed.observer());
    hook.start([](MouseButton, std::uint8_t) { return false; }, [](const MouseEvent&) {});
    hook.set_now(7.5);

    // Pressed over its own window, then an injected right click outside, then a real left click
    // outside: each released at (-30, 12).
    const auto click = [&hook](unsigned down, unsigned up, bool outside, std::uint32_t flags) {
        CHECK_FALSE(hook.send_click(down, 400, 300, outside, flags));
        CHECK_FALSE(hook.send_click(up, -30, 12, outside, flags));
    };
    click(kWmLButtonDown, kWmLButtonUp, false, 0);
    click(kWmRButtonDown, kWmRButtonUp, true, kLlmhfInjected);
    click(kWmLButtonDown, kWmLButtonUp, true, 0);

    CHECK(observed.clicks == std::vector<Click>{{-30, 12, 7.5}});
}

TEST_CASE("an injected press or release is not observed", "[click_observer]") {
    Observed observed;
    FakeMouseHook hook(observed.observer());
    hook.start({}, {});

    CHECK_FALSE(hook.send_click(kWmLButtonDown, 10, 10, true, kLlmhfInjected));
    CHECK_FALSE(hook.send_click(kWmLButtonUp, 10, 10, true));
    CHECK(observed.clicks.empty());
    // An injected press asks nothing about the window under it.
    CHECK(hook.asked().empty());

    // A real press outside survives an injected release; the real release is the click.
    CHECK_FALSE(hook.send_click(kWmLButtonDown, 10, 10, true));
    CHECK_FALSE(hook.send_click(kWmLButtonUp, 11, 11, true, kLlmhfInjected));
    CHECK(observed.clicks.empty());
    hook.set_now(3.25);
    CHECK_FALSE(hook.send_click(kWmLButtonUp, 12, 13, true));
    CHECK(observed.clicks == std::vector<Click>{{12, 13, 3.25}});
    CHECK(hook.asked() == std::vector<std::pair<int, int>>{{10, 10}});
}

TEST_CASE("only the release of a press that began over another process is a click",
          "[click_observer]") {
    Observed observed;
    FakeMouseHook hook(observed.observer());
    hook.start({}, {});

    CHECK_FALSE(hook.click(100, 200, 1.0, false));
    CHECK(observed.clicks.empty());
    CHECK_FALSE(hook.click(100, 200, 2.0, true));
    CHECK(observed.clicks == std::vector<Click>{{100, 200, 2.0}});

    // A lone release, and a second release of the same press, are no clicks.
    CHECK_FALSE(hook.send_click(kWmLButtonUp, 5, 5, true));
    CHECK(observed.clicks.size() == 1);

    // The press point decides, not the release's: a press over this app dragged out is none.
    CHECK_FALSE(hook.send_click(kWmLButtonDown, 1, 1, false));
    CHECK_FALSE(hook.send_click(kWmLButtonUp, 900, 900, true));
    CHECK(observed.clicks.size() == 1);

    // A press outside followed by a press over this app forgets the first.
    CHECK_FALSE(hook.send_click(kWmLButtonDown, 1, 1, true));
    CHECK_FALSE(hook.send_click(kWmLButtonDown, 2, 2, false));
    CHECK_FALSE(hook.send_click(kWmLButtonUp, 3, 3, true));
    CHECK(observed.clicks.size() == 1);
}

TEST_CASE("left and right clicks are observed, each matched by its own button",
          "[click_observer]") {
    Observed observed;
    FakeMouseHook hook(observed.observer());
    hook.start({}, {});

    CHECK_FALSE(hook.click(40, 50, 1.0, true, true));
    CHECK(observed.clicks == std::vector<Click>{{40, 50, 1.0}});

    // A left press does not complete on a right release, and is still waiting after it.
    CHECK_FALSE(hook.send_click(kWmLButtonDown, 1, 1, true));
    CHECK_FALSE(hook.send_click(kWmRButtonUp, 2, 2, true));
    CHECK(observed.clicks.size() == 1);
    CHECK_FALSE(hook.send_click(kWmLButtonUp, 3, 4, true));
    CHECK(observed.clicks.size() == 2);
    CHECK(observed.clicks[1] == Click{3, 4, 1.0});

    // The middle button, side buttons and moves are none of the filter's.
    for (const unsigned message : {kWmMButtonDown, kWmMButtonUp, kWmMouseMove}) {
        CHECK_FALSE(hook.send_click(message, 7, 7, true));
    }
    CHECK_FALSE(hook.send(kWmXButtonDown));
    CHECK_FALSE(hook.send(kWmXButtonUp));
    CHECK(observed.clicks.size() == 2);
    CHECK_FALSE(ClickFilter::reads(kWmMButtonDown));
    CHECK_FALSE(ClickFilter::reads(kWmXButtonUp));
}

TEST_CASE("the hook never swallows a click, even beside a claimed side button",
          "[click_observer]") {
    Observed observed;
    std::vector<MouseEvent> buttons;
    FakeMouseHook hook(observed.observer());
    hook.start([](MouseButton, std::uint8_t) { return true; },
               [&buttons](const MouseEvent& event) { buttons.push_back(event); });

    for (const unsigned message : {kWmLButtonDown, kWmLButtonUp, kWmRButtonDown, kWmRButtonUp}) {
        CHECK_FALSE(hook.send_click(message, 3, 3, true));
    }
    CHECK(observed.clicks.size() == 2);
    // The side buttons still work as shortcuts beside the observer.
    CHECK(hook.press(MouseButton::Mouse4));
    CHECK(hook.release(MouseButton::Mouse4));
    CHECK(buttons.size() == 2);
}

TEST_CASE("a negative hook code and a stopped hook observe nothing", "[click_observer]") {
    Observed observed;
    FakeMouseHook hook(observed.observer());
    CHECK_FALSE(hook.click(1, 1, 1.0, true));
    CHECK(observed.clicks.empty());

    hook.start({}, {});
    CHECK_FALSE(hook.send_click(kWmLButtonDown, 1, 1, true, 0, -1));
    CHECK_FALSE(hook.send_click(kWmLButtonUp, 1, 1, true));
    CHECK_FALSE(hook.send_click(kWmLButtonDown, 1, 1, true));
    CHECK_FALSE(hook.send_click(kWmLButtonUp, 1, 1, true, 0, -1));
    CHECK(observed.clicks.empty());
}

TEST_CASE("a new installation forgets a press made under the last one", "[click_observer]") {
    Observed observed;
    FakeMouseHook hook(observed.observer());
    hook.start({}, {});
    CHECK_FALSE(hook.send_click(kWmLButtonDown, 1, 1, true));
    hook.stop();
    hook.start({}, {});
    CHECK_FALSE(hook.send_click(kWmLButtonUp, 1, 1, true));
    CHECK(observed.clicks.empty());
}

TEST_CASE("a source made without an observer only does side buttons", "[click_observer]") {
    FakeMouseHook hook;
    hook.start({}, {});
    CHECK_FALSE(hook.click(1, 1, 1.0, true));
    CHECK(hook.asked().empty());
}

TEST_CASE("the click filter forgets a press whose owner cannot be read", "[click_observer]") {
    ClickFilter filter;
    const MonotonicClock clock = [] {
        return 9.0;
    };
    const ClickFilter::PressedOutside outside = [](int, int) {
        return true;
    };
    const ClickFilter::PressedOutside failing = [](int, int) -> bool {
        throw std::runtime_error("no window");
    };
    CHECK_FALSE(filter.observe(0, kWmRButtonDown, 1, 1, 0, outside, clock));
    CHECK(filter.pressed_outside(kWmRButtonDown));
    CHECK_FALSE(filter.observe(0, kWmRButtonDown, 1, 1, 0, failing, clock));
    CHECK_FALSE(filter.pressed_outside(kWmRButtonDown));
    CHECK_FALSE(filter.observe(0, kWmRButtonUp, 1, 1, 0, outside, clock));

    CHECK_FALSE(filter.observe(0, kWmRButtonDown, 1, 1, 0, outside, clock));
    CHECK(filter.observe(0, kWmRButtonUp, 6, 8, 0, outside, clock) == Click{6, 8, 9.0});
    CHECK_FALSE(filter.pressed_outside(kWmRButtonDown));
}

#ifdef _WIN32

#include <windows.h>

#include <deque>

#include "hotkeys/win/mouse_hook_win32.h"

namespace {

constexpr DWORD kOwnProcess = 4242;
const HHOOK kClickHandle = reinterpret_cast<HHOOK>(static_cast<std::uintptr_t>(0xC11C));

// _FakeUser32 with WindowFromPoint and GetWindowThreadProcessId: the window under a point is its
// x, and window 1 is this process's. GetMessageW runs the hook procedure on the installing
// thread, as Windows does, and returns 0 once WM_QUIT is posted.
class ClickUser32 {
public:
    MouseHookWin32 table() {
        MouseHookWin32 win32;
        win32.set_hook = [this](HOOKPROC proc) -> HHOOK {
            std::lock_guard lock(mutex_);
            proc_ = proc;
            changed_.notify_all();
            return kClickHandle;
        };
        win32.unhook = [this](HHOOK) -> BOOL {
            std::lock_guard lock(mutex_);
            proc_ = nullptr;
            return TRUE;
        };
        win32.get_message = [this](MSG*) -> BOOL {
            std::unique_lock lock(mutex_);
            for (;;) {
                changed_.wait(lock, [this] { return quit_ || !calls_.empty(); });
                if (calls_.empty()) {
                    quit_ = false;
                    return 0;
                }
                Call* call = calls_.front();
                calls_.pop_front();
                const HOOKPROC proc = proc_;
                lock.unlock();
                const LRESULT result =
                    proc(call->code, call->message, reinterpret_cast<LPARAM>(&call->info));
                lock.lock();
                call->result = result;
                changed_.notify_all();
            }
        };
        win32.post_thread_message = [this](DWORD, UINT message) -> BOOL {
            std::lock_guard lock(mutex_);
            quit_ = quit_ || message == WM_QUIT;
            changed_.notify_all();
            return TRUE;
        };
        win32.call_next = [this](int, WPARAM message, LPARAM) -> LRESULT {
            std::lock_guard lock(mutex_);
            next_calls_.push_back(static_cast<UINT>(message));
            return 0;
        };
        win32.get_async_key_state = [](int) -> SHORT {
            return 0;
        };
        win32.current_thread_id = [] {
            return DWORD{77};
        };
        win32.window_from_point = [](POINT point) {
            return reinterpret_cast<HWND>(static_cast<std::intptr_t>(point.x));
        };
        win32.window_thread_process_id = [](HWND window, DWORD* process_id) -> DWORD {
            *process_id =
                reinterpret_cast<std::intptr_t>(window) == 1 ? kOwnProcess : kOwnProcess + 1;
            return 1;
        };
        win32.current_process_id = [] {
            return kOwnProcess;
        };
        return win32;
    }

    // One hook call on the hook thread; what the procedure returned, or nullopt after 2 s.
    std::optional<LRESULT> call(UINT message, POINT at, DWORD flags = 0) {
        Call call;
        call.message = message;
        call.info.pt = at;
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
    std::vector<UINT> next_calls() {
        std::lock_guard lock(mutex_);
        return next_calls_;
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
    bool quit_ = false;
    std::deque<Call*> calls_;
    std::vector<UINT> next_calls_;
};

// Clicks the dispatch thread delivered, waited for from the test thread.
class Delivered {
public:
    void add(Click click, DWORD thread) {
        std::lock_guard lock(mutex_);
        clicks_.push_back(click);
        threads_.push_back(thread);
        changed_.notify_all();
    }
    bool wait_for(std::size_t count) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 2s, [&] { return clicks_.size() >= count; });
    }
    std::vector<Click> clicks() {
        std::lock_guard lock(mutex_);
        return clicks_;
    }
    std::vector<DWORD> threads() {
        std::lock_guard lock(mutex_);
        return threads_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Click> clicks_;
    std::vector<DWORD> threads_;
};

} // namespace

TEST_CASE("the click-observing hook passes clicks on and dispatches them off the hook thread",
          "[click_observer]") {
    ClickUser32 user32;
    Delivered delivered;
    auto hook = make_mouse_hook_source(
        [&delivered](int x, int y, double at) {
            delivered.add(Click{x, y, at}, GetCurrentThreadId());
        },
        [] { return 12.5; }, user32.table());
    // A walkthrough claims nothing and takes no side buttons.
    hook->start({}, {});
    REQUIRE(hook->wait_until_installed(2s));

    // Over its own window (x 1), injected, then over another app's window (x 2).
    CHECK(user32.call(WM_LBUTTONDOWN, POINT{1, 0}) == LRESULT{0});
    CHECK(user32.call(WM_LBUTTONUP, POINT{-30, 12}) == LRESULT{0});
    CHECK(user32.call(WM_RBUTTONDOWN, POINT{2, 0}, LLMHF_INJECTED) == LRESULT{0});
    CHECK(user32.call(WM_RBUTTONUP, POINT{-30, 12}, LLMHF_INJECTED) == LRESULT{0});
    CHECK(user32.call(WM_LBUTTONDOWN, POINT{2, 0}) == LRESULT{0});
    CHECK(user32.call(WM_LBUTTONUP, POINT{-30, 12}) == LRESULT{0});
    // A side button with nothing claimed passes as well.
    CHECK(user32.call(WM_XBUTTONDOWN, POINT{2, 0}) == LRESULT{0});
    REQUIRE(delivered.wait_for(1));
    hook->stop();

    CHECK(delivered.clicks() == std::vector<Click>{{-30, 12, 12.5}});
    CHECK(delivered.threads()[0] != GetCurrentThreadId());
    CHECK(user32.next_calls() == std::vector<UINT>{WM_LBUTTONDOWN, WM_LBUTTONUP, WM_RBUTTONDOWN,
                                                   WM_RBUTTONUP, WM_LBUTTONDOWN, WM_LBUTTONUP,
                                                   WM_XBUTTONDOWN});
}

TEST_CASE("a side-button source without an observer reads no clicks", "[click_observer]") {
    ClickUser32 user32;
    MouseHookWin32 table = user32.table();
    // Never asked: the source observes no clicks.
    table.window_from_point = nullptr;
    table.window_thread_process_id = nullptr;
    auto hook = make_mouse_hook_source([] { return 1.0; }, std::move(table));
    hook->start([](MouseButton, std::uint8_t) { return false; }, [](const MouseEvent&) {});
    REQUIRE(hook->wait_until_installed(2s));
    CHECK(user32.call(WM_LBUTTONDOWN, POINT{2, 0}) == LRESULT{0});
    CHECK(user32.call(WM_LBUTTONUP, POINT{2, 0}) == LRESULT{0});
    hook->stop();
    CHECK(user32.next_calls() == std::vector<UINT>{WM_LBUTTONDOWN, WM_LBUTTONUP});
}

// The real WH_MOUSE_LL hook with a real click: it installs a global hook and waits for the
// person at the machine, so only `windows-interactive` runs it.
TEST_CASE("the real hook observes a real click in another window", "[interactive]") {
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Click> seen;
    auto hook = make_mouse_hook_source([&](int x, int y, double at) {
        std::lock_guard lock(mutex);
        seen = Click{x, y, at};
        changed.notify_all();
    });
    hook->start({}, {});
    REQUIRE(hook->wait_until_installed(1s));
    std::printf("Left-click once in another app's window within 15 seconds.\n");
    std::unique_lock lock(mutex);
    const bool clicked = changed.wait_for(lock, 15s, [&] { return seen.has_value(); });
    lock.unlock();
    hook->stop();
    REQUIRE(clicked);
    CHECK(seen->at > 0);
}

#endif
