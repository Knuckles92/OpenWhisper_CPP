#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "hotkeys/mouse_hook.h"

namespace fv::hotkeys {

// A mouse hook driven by the test. start() and stop() count and flip `running`, as
// test_personalize_s6_backends' _FakeMouseHook does. send() is a hook call: it runs the real
// decision (MouseButtonFilter, whose swallowed set survives restarts) with the started claims,
// reads modifiers from the keys set_pressed() holds down (GetAsyncKeyState), and dispatches what
// it swallows to on_button on the calling thread, in place of the dispatch thread. Made with a
// ClickObserver, it also observes clicks as make_mouse_hook_source(on_click) does: send_click()
// and click() run the real ClickFilter (a new one each start) and hand what it keeps to the
// observer on the calling thread.
// Affinity: thread-safe.
class FakeMouseHook final : public MouseHookSource {
public:
    // Every hook call is timed as 0.5 ms, so the worst latency is known.
    FakeMouseHook()
        : filter_([this] {
              std::lock_guard lock(timer_mutex_);
              timer_ += 0.0005;
              return timer_;
          }) {}
    explicit FakeMouseHook(ClickObserver on_click) : FakeMouseHook() {
        on_click_ = std::move(on_click);
    }

    void start(Claims claims, OnButton on_button) override {
        std::lock_guard lock(mutex_);
        if (fail_next_) {
            std::string message = std::move(*fail_next_);
            fail_next_.reset();
            throw std::runtime_error(message);
        }
        if (!running_) {
            ++starts_;
            claims_ = claims ? std::move(claims)
                             : Claims([](MouseButton, std::uint8_t) { return false; });
            on_button_ = on_button ? std::move(on_button) : OnButton([](const MouseEvent&) {});
            clicks_ = ClickFilter();
        }
        running_ = true;
    }
    void stop() override {
        std::lock_guard lock(mutex_);
        if (running_) {
            ++stops_;
        }
        running_ = false;
    }
    bool running() const override {
        std::lock_guard lock(mutex_);
        return running_;
    }
    bool wait_until_installed(std::chrono::milliseconds) override { return running(); }

    // A hook call with an X button message (kWmXButtonDown or kWmXButtonUp), the X button in the
    // high word of mouseData (1 is Mouse 4) and the flags (kLlmhfInjected). True when the hook
    // swallows it. With no hook running, everything passes.
    bool send(unsigned message, unsigned xbutton = 1, std::uint32_t flags = 0, int code = 0) {
        Claims claims;
        OnButton on_button;
        {
            std::lock_guard lock(mutex_);
            if (!running_) {
                return false;
            }
            claims = claims_;
            on_button = on_button_;
        }
        std::vector<MouseEvent> dispatched;
        const bool swallow = filter_.decide(
            code, message, static_cast<std::uint32_t>(xbutton) << 16, flags,
            [this](int vk) -> short {
                std::lock_guard lock(mutex_);
                return pressed_.contains(vk) ? static_cast<short>(-32768) : short{0};
            },
            claims, [this] { return now(); },
            [&dispatched](const MouseEvent& event) { dispatched.push_back(event); });
        for (const auto& event : dispatched) {
            on_button(event);
            filter_.report_latency();
        }
        return swallow;
    }
    bool press(MouseButton button, std::uint32_t flags = 0) {
        return send(kWmXButtonDown, button == MouseButton::Mouse4 ? 1 : 2, flags);
    }
    bool release(MouseButton button, std::uint32_t flags = 0) {
        return send(kWmXButtonUp, button == MouseButton::Mouse4 ? 1 : 2, flags);
    }

    // A hook call with a left or right button message (kWmLButtonDown ... kWmRButtonUp) at screen
    // point (x, y). `outside` is what WindowFromPoint's owner says for a press: another process's
    // window. True when the hook swallows it, which it never does; with no hook running, or a
    // hook made without an observer, nothing is observed.
    bool send_click(unsigned message, int x, int y, bool outside, std::uint32_t flags = 0,
                    int code = 0) {
        Claims claims;
        ClickObserver on_click;
        {
            std::lock_guard lock(mutex_);
            if (!running_) {
                return false;
            }
            claims = claims_;
            on_click = on_click_;
        }
        const bool swallow = filter_.decide(
            code, message, 0, flags, [](int) -> short { return 0; }, claims,
            [this] { return now(); }, [](const MouseEvent&) {});
        if (!on_click) {
            return swallow;
        }
        std::optional<Click> click;
        {
            std::lock_guard lock(mutex_);
            click = clicks_.observe(
                code, message, x, y, flags,
                [this, outside](int at_x, int at_y) {
                    asked_.emplace_back(at_x, at_y);
                    return outside;
                },
                [this] { return now_; });
        }
        if (click) {
            on_click(click->x, click->y, click->at);
            filter_.report_latency();
        }
        return swallow;
    }
    // A left click (or right, with `right`) pressed and released at (x, y) at time `at`, its
    // press over another process's window when `outside`. True when the hook swallowed either
    // half.
    bool click(int x, int y, double at, bool outside, bool right = false) {
        set_now(at);
        const bool down = send_click(right ? kWmRButtonDown : kWmLButtonDown, x, y, outside);
        const bool up = send_click(right ? kWmRButtonUp : kWmLButtonUp, x, y, outside);
        return down || up;
    }
    // The points the hook asked about (WindowFromPoint), oldest first.
    std::vector<std::pair<int, int>> asked() const {
        std::lock_guard lock(mutex_);
        return asked_;
    }

    // The virtual keys GetAsyncKeyState reports down (kVkControl, kVkMenu, ...).
    void set_pressed(std::set<int> keys) {
        std::lock_guard lock(mutex_);
        pressed_ = std::move(keys);
    }
    // The time stamp the next swallowed event gets.
    void set_now(double now) {
        std::lock_guard lock(mutex_);
        now_ = now;
    }
    double now() const {
        std::lock_guard lock(mutex_);
        return now_;
    }

    // What the hook asks the owner inside the hook, and hands the dispatch thread.
    bool claims(MouseButton button, std::uint8_t modifiers) const {
        Claims claims;
        {
            std::lock_guard lock(mutex_);
            claims = claims_;
        }
        return claims && claims(button, modifiers);
    }
    void dispatch(const MouseEvent& event) const {
        OnButton on_button;
        {
            std::lock_guard lock(mutex_);
            on_button = on_button_;
        }
        if (on_button) {
            on_button(event);
        }
    }

    // The next start() throws std::runtime_error(message).
    void fail_next_start(std::string message) {
        std::lock_guard lock(mutex_);
        fail_next_ = std::move(message);
    }
    int starts() const {
        std::lock_guard lock(mutex_);
        return starts_;
    }
    int stops() const {
        std::lock_guard lock(mutex_);
        return stops_;
    }
    MouseButtonFilter& filter() { return filter_; }

private:
    mutable std::mutex mutex_;
    std::mutex timer_mutex_;
    double timer_ = 0;
    MouseButtonFilter filter_;
    Claims claims_;
    OnButton on_button_;
    ClickObserver on_click_;
    ClickFilter clicks_;
    std::vector<std::pair<int, int>> asked_;
    std::set<int> pressed_;
    double now_ = 0;
    bool running_ = false;
    int starts_ = 0;
    int stops_ = 0;
    std::optional<std::string> fail_next_;
};

} // namespace fv::hotkeys
