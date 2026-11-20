#include "hotkeys/capture.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

#include "base/log.h"
#include "hotkeys/detail/thread_name.h"
#include "hotkeys/grammar.h"
#include "hotkeys/key_names.h"
#include "hotkeys/mouse_shortcuts.h"

namespace fv::hotkeys {

namespace {

constexpr log::Logger kLog{"ui_qt.widgets.hotkey_capture"};

void replace_all(std::string& text, std::string_view from, std::string_view to) {
    for (std::size_t at = text.find(from); at != std::string::npos;
         at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
}

// get_hotkey_name's sorting key: (index among the modifiers or 5, the name).
std::pair<int, std::string_view> sorting_key(std::string_view name) {
    static constexpr std::array<std::string_view, 4> kModifiers{"ctrl", "alt", "shift", "windows"};
    const auto found = std::find(kModifiers.begin(), kModifiers.end(), name);
    return {found == kModifiers.end() ? 5 : static_cast<int>(found - kModifiers.begin()), name};
}

class HookHotkeyCapture final : public HotkeyCapture {
public:
    explicit HookHotkeyCapture(std::unique_ptr<KeyboardHook> hook) : hook_(std::move(hook)) {}
    // Subscribers run on their own executors, never inline on the capture thread, so the
    // capture is never destroyed from its own thread.
    ~HookHotkeyCapture() override {
        stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void start() override {
        std::lock_guard lock(mutex_);
        if (thread_.joinable()) {
            if (!done_) {
                return;
            }
            thread_.join();
        }
        stopped_ = false;
        done_ = false;
        events_.clear();
        thread_ = std::thread([this] { run(); });
    }

    void stop() override {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
            events_.push_back(std::nullopt);
        }
        changed_.notify_all();
    }

    bool running() const override {
        std::lock_guard lock(mutex_);
        return thread_.joinable() && !done_;
    }

    bool wait(std::chrono::milliseconds timeout) override {
        std::unique_lock lock(mutex_);
        if (!thread_.joinable()) {
            return true;
        }
        return changed_.wait_for(lock, timeout, [this] { return done_; });
    }

private:
    std::optional<CapturedKey> next() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return !events_.empty(); });
        auto event = std::move(events_.front());
        events_.pop_front();
        return event;
    }

    bool stopped() const {
        std::lock_guard lock(mutex_);
        return stopped_;
    }

    void run() {
        detail::name_current_thread("fv-hotkey-capture");
        bool hooked = false;
        try {
            std::vector<CapturedKey> keys;
            // Non-suppressing, and only this hook: the app's own suppressing shortcut hook must
            // survive a canceled capture.
            hook_->install([this](const KeyEvent& event) {
                CapturedKey key;
                if (!event.name.empty()) {
                    key.name = std::string(event.name);
                }
                key.keypad = event.keypad;
                key.down = event.down;
                {
                    std::lock_guard lock(mutex_);
                    events_.push_back(std::move(key));
                }
                changed_.notify_all();
                return true;
            });
            hooked = true;
            while (!stopped()) {
                auto event = next();
                if (!event) {
                    break;
                }
                keys.push_back(std::move(*event));
                const CaptureOutcome outcome = capture_outcome(keys);
                if (outcome.kind == CaptureOutcome::Kind::Cancelled) {
                    cancelled.emit();
                    break;
                }
                if (outcome.kind == CaptureOutcome::Kind::Captured) {
                    captured.emit(outcome.hotkey);
                    break;
                }
            }
        } catch (const std::exception& error) {
            if (!stopped()) {
                kLog.error("Error capturing hotkey: {}", error.what());
                failed.emit(std::string(kHotkeyCaptureFailureMessage));
            }
        }
        if (hooked) {
            try {
                hook_->uninstall();
            } catch (...) {
                // Hook already removed; nothing to release.
            }
        }
        // QThread emits finished before wait() returns.
        finished.emit();
        {
            std::lock_guard lock(mutex_);
            done_ = true;
        }
        changed_.notify_all();
    }

    std::unique_ptr<KeyboardHook> hook_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    // A key the hook saw, or nullopt from stop().
    std::deque<std::optional<CapturedKey>> events_;
    bool stopped_ = false;
    bool done_ = false;
    std::thread thread_;
};

} // namespace

std::string get_hotkey_name(const std::vector<std::string>& names) {
    std::set<std::string> clean;
    for (const auto& name : names) {
        std::string normalized = normalize_key_name(name);
        replace_all(normalized, "left ", "");
        replace_all(normalized, "right ", "");
        replace_all(normalized, "+", "plus");
        clean.insert(std::move(normalized));
    }
    std::vector<std::string> sorted(clean.begin(), clean.end());
    std::stable_sort(sorted.begin(), sorted.end(), [](const std::string& a, const std::string& b) {
        return sorting_key(a) < sorting_key(b);
    });
    std::string joined;
    for (const auto& name : sorted) {
        if (!joined.empty()) {
            joined += '+';
        }
        joined += name;
    }
    return joined;
}

CaptureOutcome capture_outcome(const std::vector<CapturedKey>& keys) {
    if (keys.empty() || keys.back().down) {
        return {};
    }
    if (std::all_of(keys.begin(), keys.end(), [](const CapturedKey& key) {
            return is_escape_key(key.name ? std::optional<std::string_view>(*key.name)
                                          : std::nullopt);
        })) {
        return {CaptureOutcome::Kind::Cancelled, ""};
    }
    std::vector<std::string> names;
    for (const auto& key : keys) {
        if (key.keypad) {
            // Python formats a missing name as "None".
            names.push_back("kp_" + key.name.value_or("None"));
        } else if (key.name) {
            names.push_back(*key.name);
        } else {
            throw std::invalid_argument(
                "Can only normalize non-empty string names. Unexpected None");
        }
    }
    const ParsedHotkey parsed = parse_hotkey(get_hotkey_name(names), Vocabulary::Windows);
    return {CaptureOutcome::Kind::Captured,
            format_hotkey(parsed.modifiers, parsed.main_key, Vocabulary::Windows)};
}

std::unique_ptr<HotkeyCapture> make_hotkey_capture(std::unique_ptr<KeyboardHook> hook) {
    return std::make_unique<HookHotkeyCapture>(std::move(hook));
}

std::unique_ptr<HotkeyCapture> make_hotkey_capture() {
    return make_hotkey_capture(make_capture_hook());
}

} // namespace fv::hotkeys
