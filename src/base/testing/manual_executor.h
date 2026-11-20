#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "base/executor.h"
#include "base/subscription.h"
#include "base/testing/fake_clock.h"

namespace fv {

// An executor that runs nothing until the test says so, on the test's thread: run_all() for
// posted tasks, advance() for timers. is_current() is true only while it runs a task, so a
// Signal delivers inline exactly when Python's synchronous callback would. Time is its own
// (starting at the steady epoch) or a FakeClock's; with a clock, moving the clock also runs the
// timers that fall due. A task that throws propagates to the run_all() or advance() caller.
// Affinity: post and post_after from any thread; run_all and advance from one thread at a time.
class ManualExecutor final : public Executor {
public:
    ManualExecutor() : impl_(std::make_shared<Impl>()) {}
    explicit ManualExecutor(FakeClock& clock) : ManualExecutor() {
        impl_->clock = &clock;
        std::weak_ptr<Impl> weak = impl_;
        clock_link_ = clock.on_change([weak] {
            if (auto impl = weak.lock(); impl && !impl->advancing) {
                impl->run_due(impl->now());
            }
        });
    }
    ManualExecutor(const ManualExecutor&) = delete;
    ManualExecutor& operator=(const ManualExecutor&) = delete;
    // Drops what is queued and cancels the timers.
    ~ManualExecutor() override {
        clock_link_.disconnect();
        std::map<Key, Timer> timers;
        {
            std::lock_guard lock(impl_->mutex);
            impl_->queue.clear();
            timers.swap(impl_->timers);
        }
        for (auto& entry : timers) {
            entry.second.state->cancel();
        }
    }

    void post(Task task) override {
        std::lock_guard lock(impl_->mutex);
        impl_->queue.push_back(std::move(task));
    }

    TimerHandle post_after(std::chrono::milliseconds delay, Task task) override {
        std::lock_guard lock(impl_->mutex);
        const Key key{impl_->now_locked() + std::max(delay, std::chrono::milliseconds(0)),
                      impl_->next_timer++};
        std::weak_ptr<Impl> weak = impl_;
        auto state = std::make_shared<TimerState>([weak, key] {
            if (auto impl = weak.lock()) {
                std::map<Key, Timer>::node_type node;
                std::lock_guard inner(impl->mutex);
                node = impl->timers.extract(key);
            }
        });
        impl_->timers.emplace(key, Timer{state, std::move(task)});
        return TimerHandle(std::move(state));
    }

    bool is_current() const override {
        return impl_->depth.load() > 0 && impl_->runner.load() == std::this_thread::get_id();
    }

    // Runs queued tasks, and the tasks they post, until none is left; timers wait for advance().
    // Returns how many ran.
    std::size_t run_all() { return impl_->run_all(); }
    bool run_one() { return impl_->run_one(); }

    // Moves time forward. Timers fall due in deadline order (posting order for equal deadlines),
    // each at its own deadline, so a timer re-armed by a timer fires again within the same
    // advance; queued tasks run before each timer and at the end. Returns how many tasks ran.
    std::size_t advance(std::chrono::milliseconds by) {
        impl_->advancing = true;
        struct Reset {
            Impl* impl;
            ~Reset() { impl->advancing = false; }
        } reset{impl_.get()};
        const auto target = impl_->now() + by;
        std::size_t count = impl_->run_due(target);
        impl_->move_to(target);
        count += impl_->run_all();
        return count;
    }

    std::size_t queued() const {
        std::lock_guard lock(impl_->mutex);
        return impl_->queue.size();
    }
    std::size_t pending_timers() const {
        std::lock_guard lock(impl_->mutex);
        return impl_->timers.size();
    }
    std::chrono::steady_clock::time_point now() const { return impl_->now(); }

private:
    using Key = std::pair<std::chrono::steady_clock::time_point, std::uint64_t>;
    struct Timer {
        std::shared_ptr<TimerState> state;
        Task task;
    };

    struct Impl {
        mutable std::mutex mutex;
        std::deque<Task> queue;
        std::map<Key, Timer> timers;
        std::uint64_t next_timer = 0;
        std::chrono::steady_clock::time_point own_now{};
        FakeClock* clock = nullptr;
        std::atomic<int> depth{0};
        std::atomic<std::thread::id> runner{};
        std::atomic<bool> advancing{false};

        std::chrono::steady_clock::time_point now_locked() const {
            return clock ? clock->steady() : own_now;
        }
        std::chrono::steady_clock::time_point now() const {
            std::lock_guard lock(mutex);
            return now_locked();
        }
        void move_to(std::chrono::steady_clock::time_point when) {
            if (clock) {
                const auto current = clock->steady();
                if (when > current) {
                    clock->advance(when - current);
                }
            } else {
                std::lock_guard lock(mutex);
                if (when > own_now) {
                    own_now = when;
                }
            }
        }

        void run(const Task& task) {
            const auto previous = runner.exchange(std::this_thread::get_id());
            ++depth;
            struct Restore {
                Impl* impl;
                std::thread::id previous;
                ~Restore() {
                    --impl->depth;
                    impl->runner.store(previous);
                }
            } restore{this, previous};
            task();
        }

        bool run_one() {
            Task task;
            {
                std::lock_guard lock(mutex);
                if (queue.empty()) {
                    return false;
                }
                task = std::move(queue.front());
                queue.pop_front();
            }
            run(task);
            return true;
        }

        std::size_t run_all() {
            std::size_t count = 0;
            while (run_one()) {
                ++count;
            }
            return count;
        }

        // Runs every timer due by `until`, each at its deadline.
        std::size_t run_due(std::chrono::steady_clock::time_point until) {
            std::size_t count = run_all();
            for (;;) {
                std::map<Key, Timer>::node_type node;
                {
                    std::lock_guard lock(mutex);
                    if (timers.empty() || timers.begin()->first.first > until) {
                        break;
                    }
                    node = timers.extract(timers.begin());
                }
                move_to(node.key().first);
                auto& timer = node.mapped();
                if (timer.state->fire()) {
                    run(timer.task);
                    ++count;
                }
                count += run_all();
            }
            return count;
        }
    };

    std::shared_ptr<Impl> impl_;
    Subscription clock_link_;
};

} // namespace fv
