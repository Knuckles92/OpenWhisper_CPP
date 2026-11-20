#include "base/cancel.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "base/log.h"

namespace fv {

namespace detail {

struct CancelState {
    struct Callback {
        std::uint64_t id;
        std::shared_ptr<CallbackGuard> guard;
        Task task;
    };

    std::mutex mutex;
    std::condition_variable cancelled_changed;
    std::atomic<bool> cancelled{false};
    std::vector<Callback> callbacks;
    std::uint64_t next_id = 0;
    Subscription parent; // a child source's link to its parent's token
};

} // namespace detail

namespace {

constexpr log::Logger kLog{"base.cancel"};

void run_callback(const Task& task) noexcept {
    try {
        task();
    } catch (const std::exception& error) {
        try {
            kLog.error_with(error, "A cancel callback raised an exception");
        } catch (...) {
        }
    } catch (...) {
        try {
            kLog.error("A cancel callback raised a non-standard exception");
        } catch (...) {
        }
    }
}

void cancel_state(detail::CancelState& state) {
    std::vector<detail::CancelState::Callback> callbacks;
    {
        std::lock_guard lock(state.mutex);
        if (state.cancelled.load()) {
            return;
        }
        state.cancelled.store(true);
        callbacks.swap(state.callbacks);
    }
    state.cancelled_changed.notify_all();
    for (auto& callback : callbacks) {
        callback.guard->run([&] { run_callback(callback.task); });
        callback.guard->close();
    }
}

} // namespace

CancelToken::CancelToken(std::shared_ptr<detail::CancelState> state) : state_(std::move(state)) {}

bool CancelToken::cancelled() const {
    return state_ && state_->cancelled.load();
}

bool CancelToken::wait_for(std::chrono::milliseconds timeout) const {
    if (!state_) {
        std::this_thread::sleep_for(timeout);
        return false;
    }
    std::unique_lock lock(state_->mutex);
    return state_->cancelled_changed.wait_for(lock, timeout,
                                              [&] { return state_->cancelled.load(); });
}

Subscription CancelToken::on_cancel(Task task) const {
    if (!state_) {
        return {};
    }
    auto guard = std::make_shared<CallbackGuard>();
    std::uint64_t id = 0;
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->cancelled.load()) {
            id = state_->next_id++;
            state_->callbacks.push_back({id, guard, std::move(task)});
            task = nullptr;
        }
    }
    if (task) {
        run_callback(task);
        return {};
    }
    std::weak_ptr<detail::CancelState> weak = state_;
    return Subscription(std::move(guard), [weak, id] {
        auto state = weak.lock();
        if (!state) {
            return;
        }
        detail::CancelState::Callback removed;
        {
            std::lock_guard lock(state->mutex);
            const auto it = std::find_if(
                state->callbacks.begin(), state->callbacks.end(),
                [id](const detail::CancelState::Callback& entry) { return entry.id == id; });
            if (it != state->callbacks.end()) {
                removed = std::move(*it);
                state->callbacks.erase(it);
            }
        }
        // `removed` is destroyed here, outside the lock: the task's captures may cancel.
    });
}

CancelSource::CancelSource() : state_(std::make_shared<detail::CancelState>()) {}

CancelSource::CancelSource(const CancelToken& parent) : CancelSource() {
    std::weak_ptr<detail::CancelState> weak = state_;
    auto link = parent.on_cancel([weak] {
        if (auto state = weak.lock()) {
            cancel_state(*state);
        }
    });
    std::lock_guard lock(state_->mutex);
    state_->parent = std::move(link);
}

CancelToken CancelSource::token() const {
    return CancelToken(state_);
}

void CancelSource::cancel() {
    cancel_state(*state_);
}

bool CancelSource::cancelled() const {
    return state_->cancelled.load();
}

} // namespace fv
