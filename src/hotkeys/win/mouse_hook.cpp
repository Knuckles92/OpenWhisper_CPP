#include "hotkeys/mouse_hook.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <iterator>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "base/log.h"
#include "hotkeys/detail/thread_name.h"
#include "hotkeys/mouse_shortcuts.h"
#include "hotkeys/win/mouse_hook_win32.h"

namespace fv::hotkeys {

namespace {

constexpr log::Logger kLog{"services._mouse_hook_win"};

// MouseButtonHook.stop's join timeout.
constexpr auto kStopTimeout = std::chrono::seconds(1);

// Side-button events waiting for the dispatch thread. The hook writes into preallocated slots, so
// it never allocates; a user cannot click a side button 64 times faster than one dispatch.
constexpr std::size_t kQueueSlots = 64;

// What a retired run asks in place of its owner, built once so the hook never allocates it.
const MouseHookSource::Claims kClaimsNothing = [](MouseButton, std::uint8_t) {
    return false;
};

class Flag {
public:
    void set() {
        {
            std::lock_guard lock(mutex_);
            set_ = true;
        }
        changed_.notify_all();
    }
    bool wait_for(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return set_; });
    }
    bool is_set() {
        std::lock_guard lock(mutex_);
        return set_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool set_ = false;
};

// What the hook hands the dispatch thread: a side-button event or an observed click.
struct Queued {
    bool is_click = false;
    MouseEvent event{};
    Click click{};
};

// One installation (_Run): its hook thread, dispatch queue and callbacks.
struct Run {
    std::shared_ptr<const MouseHookWin32> win32;
    MonotonicClock clock;
    MouseHookSource::Claims claims;
    MouseHookSource::OnButton on_button;
    MouseButtonFilter::KeyState key_state;
    MouseButtonFilter::Dispatch dispatch;
    // Empty unless the source observes clicks; then the hook thread alone touches `clicks`.
    ClickObserver on_click;
    ClickFilter clicks;
    ClickFilter::PressedOutside pressed_outside;

    Flag ready;
    Flag hook_done;
    Flag dispatch_done;
    std::atomic<bool> installed{false};
    std::atomic<bool> alive{false};
    // Set first thing in stop(): from then on the hook claims no press, so a hook thread a
    // timed-out stop leaves behind never asks an owner that may be gone, and a hook installed
    // after stop() gave up waiting is removed at once.
    std::atomic<bool> retired{false};
    std::atomic<DWORD> thread_id{0};
    std::thread hook_thread;
    std::thread dispatch_thread;

    std::mutex queue_mutex;
    std::condition_variable queue_changed;
    std::array<Queued, kQueueSlots> queue{};
    std::size_t head = 0;
    std::size_t count = 0;
    bool closed = false;

    // From the hook: a full queue drops the event (the press is still swallowed).
    void push(const Queued& item) {
        {
            std::lock_guard lock(queue_mutex);
            if (closed || count == queue.size()) {
                return;
            }
            queue[(head + count) % queue.size()] = item;
            ++count;
        }
        queue_changed.notify_one();
    }
    void push(const MouseEvent& event) { push(Queued{false, event, Click{}}); }
    void push(const Click& click) { push(Queued{true, MouseEvent{}, click}); }

    // Ends the dispatch thread after the events queued so far (the None MouseButtonHook.stop
    // puts on its queue).
    void close() {
        {
            std::lock_guard lock(queue_mutex);
            closed = true;
        }
        queue_changed.notify_one();
    }

    // From the source's destructor: the dispatch thread ends after the callback running now, and
    // the events still queued reach no one, since the owner they would call is going away.
    void discard() {
        {
            std::lock_guard lock(queue_mutex);
            closed = true;
            count = 0;
        }
        queue_changed.notify_one();
    }

    std::optional<Queued> pop() {
        std::unique_lock lock(queue_mutex);
        queue_changed.wait(lock, [this] { return count > 0 || closed; });
        if (count == 0) {
            return std::nullopt;
        }
        const Queued item = queue[head];
        head = (head + 1) % queue.size();
        --count;
        return item;
    }
};

class WinMouseHookSource final : public MouseHookSource {
public:
    WinMouseHookSource(MonotonicClock clock, MouseHookWin32 win32, ClickObserver on_click = {})
        : clock_(std::move(clock)),
          win32_(std::make_shared<const MouseHookWin32>(std::move(win32))),
          filter_(std::make_shared<MouseButtonFilter>()), on_click_(std::move(on_click)) {}
    // Waits for the callback each dispatch thread a stop() left running is in, and delivers
    // nothing queued behind it. The wait has no time limit, as the keyboard hook's uninstall
    // has none: the callbacks call into the owner, which goes away after this, so a thread
    // cannot be let go while one runs. A callback still running after 1 s is logged.
    ~WinMouseHookSource() override {
        stop();
        std::vector<std::shared_ptr<Run>> lingering;
        {
            std::lock_guard lock(mutex_);
            lingering = std::move(lingering_);
        }
        for (const auto& run : lingering) {
            run->discard();
            if (run->dispatch_thread.get_id() == std::this_thread::get_id()) {
                run->dispatch_thread.detach();
                continue;
            }
            if (!run->dispatch_done.wait_for(kStopTimeout)) {
                kLog.warning("A mouse button callback is still running; waiting for it before "
                             "the mouse hook is removed");
            }
            run->dispatch_thread.join();
        }
    }

    void start(Claims claims, OnButton on_button) override {
        std::lock_guard lock(mutex_);
        if (run_) {
            return;
        }
        auto run = std::make_shared<Run>();
        run->win32 = win32_;
        run->clock = clock_;
        run->claims = claims ? std::move(claims) : kClaimsNothing;
        run->on_button = std::move(on_button);
        run->key_state = [win32 = win32_.get()](int vk) {
            return win32->get_async_key_state(vk);
        };
        Run* raw = run.get();
        run->dispatch = [raw](const MouseEvent& event) {
            raw->push(event);
        };
        if (on_click_) {
            run->on_click = on_click_;
            const DWORD process =
                win32_->current_process_id ? win32_->current_process_id() : GetCurrentProcessId();
            run->pressed_outside = [win32 = win32_.get(), process](int x, int y) {
                const HWND window = win32->window_from_point(POINT{x, y});
                DWORD owner = 0;
                return window != nullptr && win32->window_thread_process_id(window, &owner) != 0 &&
                       owner != process;
            };
        }
        run->dispatch_thread =
            std::thread([run, filter = filter_] { dispatch_loop(*run, *filter); });
        run->alive = true;
        run->hook_thread = std::thread([run, filter = filter_] { hook_loop(*run, *filter); });
        run_ = std::move(run);
    }

    void stop() override {
        std::shared_ptr<Run> run;
        {
            std::lock_guard lock(mutex_);
            run = std::exchange(run_, nullptr);
        }
        if (!run) {
            return;
        }
        run->retired = true;
        run->close();
        // The hook thread names itself and makes its message queue before `ready`, so once it is
        // set the WM_QUIT lands.
        if (run->ready.wait_for(kStopTimeout) && run->thread_id.load() != 0) {
            run->win32->post_thread_message(run->thread_id.load(), WM_QUIT);
        }
        if (run->hook_done.wait_for(kStopTimeout)) {
            run->hook_thread.join();
        } else {
            // Python's join(timeout) leaves a stuck daemon thread behind; so does this. It holds
            // the run, so nothing it touches goes away, and `retired` keeps it from deciding.
            run->hook_thread.detach();
        }
        // Python never waits for its dispatch thread. This waits up to 1 s, so a stop normally
        // returns with every queued event delivered; a callback still running then (one that
        // waits on this caller, say a GUI thread changing the shortcuts) is left to finish, as
        // is the thread a stop from inside a callback runs on. The source keeps such threads and
        // its destructor joins them, because they call into the owner.
        if (run->dispatch_thread.get_id() != std::this_thread::get_id() &&
            run->dispatch_done.wait_for(kStopTimeout)) {
            run->dispatch_thread.join();
        } else {
            std::lock_guard lock(mutex_);
            lingering_.push_back(std::move(run));
        }
        join_finished_dispatches();
    }

    bool running() const override {
        std::lock_guard lock(mutex_);
        return run_ && run_->alive.load();
    }

    bool wait_until_installed(std::chrono::milliseconds timeout) override {
        std::shared_ptr<Run> run;
        {
            std::lock_guard lock(mutex_);
            run = run_;
        }
        return run && run->ready.wait_for(timeout) && run->installed.load();
    }

private:
    static LRESULT CALLBACK proc(int code, WPARAM wparam, LPARAM lparam) {
        const Context* context = current_;
        if (context == nullptr) {
            return CallNextHookEx(nullptr, code, wparam, lparam);
        }
        Run& run = *context->run;
        const auto message = static_cast<unsigned>(wparam);
        if (wparam == WM_XBUTTONDOWN || wparam == WM_XBUTTONUP) {
            const auto& info = *reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
            // A retired run still swallows the release of a press it swallowed, as Python's hook
            // does until it is removed, but asks its owner about no new press.
            const Claims& claims = run.retired.load() ? kClaimsNothing : run.claims;
            if (context->filter->decide(code, message, static_cast<std::uint32_t>(info.mouseData),
                                        static_cast<std::uint32_t>(info.flags), run.key_state,
                                        claims, run.clock, run.dispatch)) {
                return 1;
            }
        } else if (run.on_click && code >= 0 && ClickFilter::reads(message)) {
            const auto& info = *reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
            if (const auto click = run.clicks.observe(code, message, info.pt.x, info.pt.y,
                                                      static_cast<std::uint32_t>(info.flags),
                                                      run.pressed_outside, run.clock)) {
                run.push(*click);
            }
        }
        // A click always goes on to the app.
        return run.win32->call_next(code, wparam, lparam);
    }

    // The thread holds the run and the filter, so a thread a timed-out stop leaves behind never
    // outlives what it touches.
    static void hook_loop(Run& run, MouseButtonFilter& filter) {
        detail::name_current_thread("fv-mouse-hook");
        const MouseHookWin32& win32 = *run.win32;
        MSG message;
        // A thread has no message queue until it asks for a message, and
        // PostThreadMessageW(WM_QUIT) fails without one.
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        run.thread_id = win32.current_thread_id();
        const Context context{&run, &filter};
        current_ = &context;
        HHOOK handle = win32.set_hook(&WinMouseHookSource::proc);
        const DWORD error = handle == nullptr ? GetLastError() : 0;
        run.installed = handle != nullptr;
        run.ready.set();
        if (handle == nullptr) {
            kLog.warning("Mouse button shortcuts unavailable (error {})", error);
        } else {
            kLog.info("Mouse button hook installed");
            // A stop() that gave up waiting for `ready` posted no WM_QUIT; it set `retired`
            // before waiting, so this sees it.
            if (!run.retired.load()) {
                while (win32.get_message(&message) > 0) {
                }
            }
            win32.unhook(handle);
            kLog.info("Mouse button hook removed");
        }
        current_ = nullptr;
        run.alive = false;
        run.hook_done.set();
    }

    static void dispatch_loop(Run& run, MouseButtonFilter& filter) {
        detail::name_current_thread("fv-mouse-dispatch");
        while (const auto item = run.pop()) {
            try {
                if (item->is_click) {
                    run.on_click(item->click.x, item->click.y, item->click.at);
                } else if (run.on_button) {
                    run.on_button(item->event);
                }
            } catch (const std::exception& error) {
                kLog.error_with(error, "Mouse event dispatch failed");
            } catch (...) {
                kLog.error("Mouse event dispatch failed");
            }
            filter.report_latency();
        }
        run.dispatch_done.set();
    }

    // Joins the dispatch threads earlier stops left running that have ended since, so a source
    // restarted many times does not keep them all.
    void join_finished_dispatches() {
        std::vector<std::shared_ptr<Run>> finished;
        {
            std::lock_guard lock(mutex_);
            const auto ended = std::stable_partition(
                lingering_.begin(), lingering_.end(), [](const std::shared_ptr<Run>& run) {
                    return !run->dispatch_done.is_set() ||
                           run->dispatch_thread.get_id() == std::this_thread::get_id();
                });
            finished.assign(std::make_move_iterator(ended),
                            std::make_move_iterator(lingering_.end()));
            lingering_.erase(ended, lingering_.end());
        }
        // A thread that set dispatch_done has only its return left, so this join is short.
        for (const auto& run : finished) {
            run->dispatch_thread.join();
        }
    }

    // What the hook procedure needs; it has no context argument and runs only on the installing
    // thread.
    struct Context {
        Run* run;
        MouseButtonFilter* filter;
    };
    static thread_local const Context* current_;

    MonotonicClock clock_;
    std::shared_ptr<const MouseHookWin32> win32_;
    std::shared_ptr<MouseButtonFilter> filter_;
    ClickObserver on_click_;
    mutable std::mutex mutex_;
    std::shared_ptr<Run> run_;
    // Stopped runs whose dispatch thread had not ended when stop() returned.
    std::vector<std::shared_ptr<Run>> lingering_;
};

thread_local const WinMouseHookSource::Context* WinMouseHookSource::current_ = nullptr;

} // namespace

MouseHookWin32 real_mouse_hook_win32() {
    MouseHookWin32 win32;
    win32.set_hook = [](HOOKPROC proc) {
        return SetWindowsHookExW(WH_MOUSE_LL, proc, GetModuleHandleW(nullptr), 0);
    };
    win32.unhook = [](HHOOK hook) {
        return UnhookWindowsHookEx(hook);
    };
    win32.get_message = [](MSG* message) {
        return GetMessageW(message, nullptr, 0, 0);
    };
    win32.post_thread_message = [](DWORD thread_id, UINT message) {
        return PostThreadMessageW(thread_id, message, 0, 0);
    };
    win32.call_next = [](int code, WPARAM wparam, LPARAM lparam) {
        return CallNextHookEx(nullptr, code, wparam, lparam);
    };
    win32.get_async_key_state = [](int vk) {
        return GetAsyncKeyState(vk);
    };
    win32.current_thread_id = [] {
        return GetCurrentThreadId();
    };
    win32.window_from_point = [](POINT point) {
        return WindowFromPoint(point);
    };
    win32.window_thread_process_id = [](HWND window, DWORD* process_id) {
        return GetWindowThreadProcessId(window, process_id);
    };
    win32.current_process_id = [] {
        return GetCurrentProcessId();
    };
    return win32;
}

std::unique_ptr<MouseHookSource> make_mouse_hook_source(MonotonicClock clock,
                                                        MouseHookWin32 win32) {
    return std::make_unique<WinMouseHookSource>(std::move(clock), std::move(win32));
}

std::unique_ptr<MouseHookSource> make_mouse_hook_source(MonotonicClock clock) {
    return make_mouse_hook_source(std::move(clock), real_mouse_hook_win32());
}

std::unique_ptr<MouseHookSource>
make_mouse_hook_source(ClickObserver on_click, MonotonicClock clock, MouseHookWin32 win32) {
    return std::make_unique<WinMouseHookSource>(std::move(clock), std::move(win32),
                                                std::move(on_click));
}

std::unique_ptr<MouseHookSource> make_mouse_hook_source(ClickObserver on_click,
                                                        MonotonicClock clock) {
    return make_mouse_hook_source(std::move(on_click), std::move(clock), real_mouse_hook_win32());
}

bool mouse_shortcuts_supported() {
    return true;
}

} // namespace fv::hotkeys
