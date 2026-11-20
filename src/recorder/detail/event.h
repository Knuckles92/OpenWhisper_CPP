#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

// threading.Event: a flag threads wait on.
namespace fv::recorder_detail {

// Affinity: thread-safe.
class Event {
public:
    void set() {
        {
            const std::lock_guard lock(mutex_);
            flag_ = true;
        }
        changed_.notify_all();
    }
    void clear() {
        const std::lock_guard lock(mutex_);
        flag_ = false;
    }
    bool is_set() const {
        const std::lock_guard lock(mutex_);
        return flag_;
    }
    // True when the flag is set, at once or within `timeout`.
    template <class Rep, class Period>
    bool wait_for(std::chrono::duration<Rep, Period> timeout) const {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return flag_; });
    }
    void wait() const {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return flag_; });
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    bool flag_ = false;
};

} // namespace fv::recorder_detail
