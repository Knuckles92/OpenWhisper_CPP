#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/executor.h"
#include "base/signal.h"
#include "dictation/hotkey_runtime.h"
#include "dictation/recording_control.h"
#include "focus/testing/fake_synthetic_keys.h"
#include "hotkeys/hotkey_manager.h"
#include "hotkeys/testing/fake_keyboard_hook.h"
#include "settings/testing/memory_store.h"

// The fixtures of OpenWhisper's HotkeyRuntime tests (tests/test_personalize_s6_runtime.py
// _Controller and _drain, tests/test_push_hold_recording.py _FakeController): a recording side
// that records what it is asked, a UI executor that runs everything at once on the calling
// thread, and a runtime over a HotkeyManager on a fake hook.
namespace fv::dictation::testing {

// Runs a posted task at once on the posting thread, and counts as current everywhere, so the
// runtime's UI calls happen inline on the dispatcher as Python's direct calls did. Timers are
// kept until fire_timers() runs them.
// Affinity: thread-safe.
class InlineExecutor final : public Executor {
public:
    void post(Task task) override { task(); }
    TimerHandle post_after(std::chrono::milliseconds delay, Task task) override {
        auto state = std::make_shared<TimerState>();
        std::lock_guard lock(mutex_);
        timers_.push_back({delay, state, std::move(task)});
        return TimerHandle(state);
    }
    bool is_current() const override { return true; }

    // Runs the timers pending now (not the ones they arm); returns how many ran.
    std::size_t fire_timers() {
        std::vector<Timer> due;
        {
            std::lock_guard lock(mutex_);
            due.swap(timers_);
        }
        std::size_t count = 0;
        for (auto& timer : due) {
            if (timer.state->fire()) {
                timer.task();
                ++count;
            }
        }
        return count;
    }
    // The delays of the timers armed and still pending.
    std::vector<std::chrono::milliseconds> pending_delays() const {
        std::lock_guard lock(mutex_);
        std::vector<std::chrono::milliseconds> delays;
        for (const auto& timer : timers_) {
            if (timer.state->pending()) {
                delays.push_back(timer.delay);
            }
        }
        return delays;
    }

private:
    struct Timer {
        std::chrono::milliseconds delay;
        std::shared_ptr<TimerState> state;
        Task task;
    };
    mutable std::mutex mutex_;
    std::vector<Timer> timers_;
};

// Records what the runtime asks for. As _Controller, a stop or cancel ends a recording that is
// running and says so (ends_recordings); as _FakeController, it does not.
// Affinity: thread-safe.
class FakeRecordingControl final : public RecordingControl {
public:
    bool is_recording() const override { return recording.load(); }
    bool recorder_busy() const override { return busy.load(); }
    bool canceling_recording() const override { return false; }
    bool capture_canceled() const override { return canceled.load(); }
    bool stream_open() const override { return stream.load(); }

    bool start(std::string source) override {
        record("start");
        {
            std::lock_guard lock(mutex_);
            sources_.push_back(std::move(source));
        }
        if (before_start) {
            before_start();
        }
        const bool accepted = accept.load();
        if (accepted && opens_stream.load()) {
            recording = true;
            stream = true;
            changed_.emit(true);
        } else if (!accepted) {
            recording = false;
        }
        return accepted;
    }
    void stop() override {
        record("stop");
        ended();
    }
    bool toggle() override {
        record("toggle");
        return true;
    }
    bool cancel() override {
        record("cancel");
        ended();
        return true;
    }
    void minimize_to_tray() override { record("minimize"); }
    void paste_last_original() override {
        // Before it is recorded, so a test that waits for the call also sees what on_paste did.
        if (on_paste) {
            on_paste();
        }
        record("paste");
    }
    Signal<bool>& recording_state_changed() override { return changed_; }

    std::vector<std::string> calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }
    std::vector<std::string> sources() const {
        std::lock_guard lock(mutex_);
        return sources_;
    }
    // Whether each call ran where `ui` is current (set ui first).
    std::vector<bool> on_ui() const {
        std::lock_guard lock(mutex_);
        return on_ui_;
    }
    // Waits until `calls()` has `count` entries.
    bool wait_for_calls(std::size_t count,
                        std::chrono::milliseconds timeout = std::chrono::seconds(2)) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (calls().size() < count) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }

    std::atomic<bool> recording{false};
    std::atomic<bool> busy{false};
    std::atomic<bool> canceled{false};
    std::atomic<bool> stream{false};
    std::atomic<bool> accept{true};
    // A start opens the stream at once; false models a start whose stream never opens.
    std::atomic<bool> opens_stream{true};
    std::atomic<bool> ends_recordings{true};
    // Set before use.
    std::function<void()> before_start;
    std::function<void()> on_paste;
    const Executor* ui = nullptr;

private:
    void record(const std::string& call) {
        std::lock_guard lock(mutex_);
        calls_.push_back(call);
        on_ui_.push_back(ui != nullptr && ui->is_current());
    }
    void ended() {
        if (ends_recordings.load() && recording.exchange(false)) {
            stream = false;
            changed_.emit(false);
        }
    }

    mutable std::mutex mutex_;
    std::vector<std::string> calls_;
    std::vector<std::string> sources_;
    std::vector<bool> on_ui_;
    Signal<bool> changed_;
};

// Collects what a signal emits.
// Affinity: thread-safe.
template <class T> class Emitted {
public:
    Emitted(Signal<T>& signal, Executor& executor)
        : link_(signal.connect(executor, [this](T value) {
              std::lock_guard lock(mutex_);
              values_.push_back(std::move(value));
          })) {}
    std::vector<T> values() const {
        std::lock_guard lock(mutex_);
        return values_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<T> values_;
    Subscription link_;
};

// Seconds on the hotkeys' monotonic clock (Python's time.monotonic()).
inline double monotonic_now() {
    return hotkeys::monotonic_clock()();
}

// A HotkeyRuntime over a HotkeyManager on a FakeKeyboardHook, with fakes for everything else.
struct Rig {
    explicit Rig(std::map<std::string, std::string> hotkeys = {{"record_toggle", "f9"}},
                 Executor* ui_executor = nullptr, Clock& clock = system_clock(),
                 bool wayland = false)
        : ui(ui_executor != nullptr ? *ui_executor : inline_ui) {
        auto fake = std::make_unique<hotkeys::FakeKeyboardHook>();
        hook = fake.get();
        manager = std::make_unique<hotkeys::HotkeyManager>(std::move(hotkeys), std::move(fake));
        control.ui = &ui;
        runtime = std::make_unique<HotkeyRuntime>(*manager, control, settings, ui, clock, keys,
                                                  [wayland] { return wayland; });
        hands_free = std::make_unique<Emitted<bool>>(runtime->hands_free_changed, inline_ui);
        statuses = std::make_unique<Emitted<std::string>>(runtime->status, inline_ui);
    }
    ~Rig() {
        runtime->cleanup();
        runtime.reset();
    }

    // _drain: everything queued for the dispatcher before now has run, and what that queued.
    void drain() {
        for (int round = 0; round < 2; ++round) {
            auto done = std::make_shared<std::promise<void>>();
            auto finished = done->get_future();
            runtime->dispatcher().submit([done] { done->set_value(); });
            if (finished.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
                throw std::runtime_error("the hotkey dispatcher did not drain");
            }
        }
    }
    // _tap_then_press: taps the record key (100 ms), then presses it again `gap` s later.
    double tap_then_press(double gap) {
        const double start = monotonic_now();
        runtime->queue_record_press(start);
        runtime->queue_record_release(start + 0.1);
        runtime->queue_record_press(start + 0.1 + gap);
        drain();
        return start + 0.1 + gap;
    }
    void set_latch(bool on) { settings.update(Json{{"recording_hands_free_latch", on}}); }

    InlineExecutor inline_ui;
    Executor& ui;
    hotkeys::FakeKeyboardHook* hook = nullptr;
    std::unique_ptr<hotkeys::HotkeyManager> manager;
    FakeRecordingControl control;
    MemorySettingsStore settings;
    focus::FakeSyntheticKeys keys;
    std::unique_ptr<HotkeyRuntime> runtime;
    std::unique_ptr<Emitted<bool>> hands_free;
    std::unique_ptr<Emitted<std::string>> statuses;
};

// Polls until `condition` holds, as _wait_until does.
inline bool wait_until(const std::function<bool()>& condition,
                       std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// A keyboard event as the hook stamps it now.
inline hotkeys::KeyEvent key_event(bool down, std::string_view name, bool keypad = true,
                                   std::uint8_t modifiers = 0) {
    hotkeys::KeyEvent event;
    event.down = down;
    event.name = name;
    event.keypad = keypad;
    event.t = monotonic_now();
    event.modifiers = modifiers;
    return event;
}

} // namespace fv::dictation::testing
