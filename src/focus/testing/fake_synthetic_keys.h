#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "focus/synthetic_keys.h"

namespace fv::focus {

// Records the keystrokes it is asked to send instead of sending them. held answers
// modifiers_held; released answers wait_for_modifiers_released (each call takes the next
// scripted answer, then the default). fail makes send_paste and send_copy throw.
// Affinity: thread-safe.
class FakeSyntheticKeys final : public SyntheticKeys {
public:
    void send_paste() override { send("paste"); }
    void send_copy() override { send("copy"); }

    bool modifiers_held() override { return held.load(); }

    bool wait_for_modifiers_released(std::chrono::milliseconds timeout = MODIFIER_WAIT,
                                     const CancelToken& cancel = {}) override {
        std::lock_guard lock(mutex_);
        waits.push_back(timeout);
        if (cancel.cancelled()) {
            return false;
        }
        if (!released_answers.empty()) {
            const bool answer = released_answers.front();
            released_answers.pop_front();
            return answer;
        }
        return released_by_default;
    }

    std::vector<std::string> sent() const {
        std::lock_guard lock(mutex_);
        return sent_;
    }

    std::atomic<bool> held{false};
    std::atomic<bool> fail{false};
    // Guarded by the fake's lock while it runs; set before use.
    std::deque<bool> released_answers;
    bool released_by_default = true;
    std::vector<std::chrono::milliseconds> waits;
    // Runs inside send_paste and send_copy before they record (to observe ordering).
    std::function<void(const std::string&)> on_send;

private:
    void send(const std::string& what) {
        if (on_send) {
            on_send(what);
        }
        if (fail) {
            throw SyntheticKeysError("This desktop can't send keystrokes");
        }
        std::lock_guard lock(mutex_);
        sent_.push_back(what);
    }

    mutable std::mutex mutex_;
    std::vector<std::string> sent_;
};

} // namespace fv::focus
