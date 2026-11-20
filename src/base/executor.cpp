#include "base/executor.h"

#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "base/detail/run_task.h"
#include "base/detail/thread_name.h"
#include "base/log.h"

namespace fv {

namespace {

constexpr int kPending = 0;
constexpr int kFired = 1;
constexpr int kCancelled = 2;

constexpr log::Logger kLog{"base.executor"};

std::atomic<std::thread::id> g_gui_thread{};

} // namespace

// --- TimerState and TimerHandle ---------------------------------------------------------------

TimerState::TimerState(std::function<void()> on_cancel) : on_cancel_(std::move(on_cancel)) {}

bool TimerState::fire() {
    int expected = kPending;
    return state_.compare_exchange_strong(expected, kFired);
}

bool TimerState::cancel() {
    int expected = kPending;
    if (!state_.compare_exchange_strong(expected, kCancelled)) {
        return false;
    }
    if (on_cancel_) {
        on_cancel_();
    }
    return true;
}

bool TimerState::pending() const {
    return state_.load() == kPending;
}

TimerHandle::TimerHandle(std::shared_ptr<TimerState> state) : state_(std::move(state)) {}

TimerHandle::TimerHandle(TimerHandle&& other) noexcept : state_(std::move(other.state_)) {}

TimerHandle& TimerHandle::operator=(TimerHandle&& other) noexcept {
    if (this != &other) {
        cancel();
        state_ = std::move(other.state_);
    }
    return *this;
}

TimerHandle::~TimerHandle() {
    cancel();
}

void TimerHandle::cancel() {
    if (auto state = std::move(state_)) {
        state->cancel();
    }
}

bool TimerHandle::active() const {
    return state_ && state_->pending();
}

void TimerHandle::detach() {
    state_.reset();
}

namespace detail {

void run_task(const Task& task, std::string_view executor_name) noexcept {
    try {
        task();
    } catch (const std::exception& error) {
        try {
            kLog.error_with(error, std::format("A task on {} raised an exception", executor_name));
        } catch (...) {
        }
    } catch (...) {
        try {
            kLog.error("A task on {} raised a non-standard exception", executor_name);
        } catch (...) {
        }
    }
}

namespace {

using SteadyTime = std::chrono::steady_clock::time_point;
using TimerKey = std::pair<SteadyTime, std::uint64_t>; // deadline, then posting order
using TaskFactory = std::function<Task(const std::shared_ptr<TimerState>&)>;

thread_local const PoolImpl* tls_pool = nullptr;
thread_local const StrandImpl* tls_strand = nullptr;

SteadyTime deadline_after(std::chrono::milliseconds delay) {
    return std::chrono::steady_clock::now() + std::max(delay, std::chrono::milliseconds(0));
}

} // namespace

// The state of a ThreadPool. The worker threads hold it too, so threads left running after a
// bounded shutdown never outlive it.
class PoolImpl : public std::enable_shared_from_this<PoolImpl> {
public:
    explicit PoolImpl(std::string name) : name_(std::move(name)) {}

    void start(int threads) {
        std::lock_guard lock(mutex_);
        alive_ = threads;
        for (int index = 0; index < threads; ++index) {
            threads_.emplace_back([self = shared_from_this(), index] { self->work(index); });
        }
    }

    bool post(Task task) {
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return false;
            }
            queue_.push_back(std::move(task));
        }
        wake_.notify_one();
        return true;
    }

    // The factory builds the task the timer runs from the timer's state, under the pool's lock,
    // so a zero delay cannot fire before the task knows its state. nullptr once shut down.
    std::shared_ptr<TimerState> add_timer(std::chrono::milliseconds delay,
                                          const TaskFactory& make) {
        std::shared_ptr<TimerState> state;
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return nullptr;
            }
            const TimerKey key{deadline_after(delay), next_timer_++};
            std::weak_ptr<PoolImpl> weak = weak_from_this();
            state = std::make_shared<TimerState>([weak, key] {
                if (auto self = weak.lock()) {
                    self->drop_timer(key);
                }
            });
            timers_.emplace(key, Timer{state, make(state)});
        }
        wake_.notify_one();
        return state;
    }

    bool is_current() const { return tls_pool == this; }

    // bounded: wait at most the budget, letting queued tasks run; otherwise drop the queue and
    // wait for the running tasks however long they take (the destructor).
    void shutdown(std::chrono::milliseconds budget, bool bounded) {
        const auto deadline = deadline_after(budget);
        const int own = is_current() ? 1 : 0;
        std::map<TimerKey, Timer> timers;
        std::deque<Task> dropped;
        std::vector<std::thread> threads;
        bool exited = false;
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            timers.swap(timers_);
        }
        wake_.notify_all();
        for (auto& entry : timers) {
            entry.second.state->cancel();
        }
        timers.clear();
        {
            std::unique_lock lock(mutex_);
            if (bounded) {
                idle_.wait_until(lock, deadline, [&] { return queue_.empty() && busy_ <= own; });
            }
            dropped.swap(queue_);
        }
        dropped.clear();
        wake_.notify_all();
        {
            std::unique_lock lock(mutex_);
            const auto exited_all = [&] {
                return alive_ <= own;
            };
            if (bounded) {
                idle_.wait_until(lock, deadline, exited_all);
            } else {
                idle_.wait(lock, exited_all);
            }
            exited = exited_all();
            threads.swap(threads_);
        }
        for (auto& thread : threads) {
            if (exited && thread.get_id() != std::this_thread::get_id()) {
                thread.join();
            } else {
                thread.detach();
            }
        }
    }

private:
    struct Timer {
        std::shared_ptr<TimerState> state;
        Task task;
    };

    void drop_timer(const TimerKey& key) {
        std::map<TimerKey, Timer>::node_type node;
        {
            std::lock_guard lock(mutex_);
            node = timers_.extract(key);
        }
        // The task's captures are destroyed here, outside the lock: a destructor may post.
    }

    void work(int index) {
        set_current_thread_name(name_ + "-" + std::to_string(index));
        tls_pool = this;
        std::unique_lock lock(mutex_);
        for (;;) {
            Task task;
            if (!timers_.empty() &&
                timers_.begin()->first.first <= std::chrono::steady_clock::now()) {
                task = std::move(timers_.begin()->second.task);
                timers_.erase(timers_.begin());
            } else if (!queue_.empty()) {
                task = std::move(queue_.front());
                queue_.pop_front();
            } else if (closed_) {
                break;
            } else if (!timers_.empty()) {
                // A copy: wait_until reads the deadline again after it wakes, and a cancel may
                // have dropped the timer whose map key this is meanwhile.
                const SteadyTime due = timers_.begin()->first.first;
                wake_.wait_until(lock, due);
                continue;
            } else {
                wake_.wait(lock);
                continue;
            }
            ++busy_;
            lock.unlock();
            run_task(task, name_);
            task = nullptr;
            lock.lock();
            --busy_;
            idle_.notify_all();
        }
        --alive_;
        idle_.notify_all();
    }

    const std::string name_;
    std::mutex mutex_;
    std::condition_variable wake_; // workers: work or a nearer timer arrived
    std::condition_variable idle_; // shutdown: a task ended or a worker exited
    std::deque<Task> queue_;
    std::map<TimerKey, Timer> timers_;
    std::uint64_t next_timer_ = 0;
    std::vector<std::thread> threads_;
    int busy_ = 0;
    int alive_ = 0;
    bool closed_ = false;
};

// The state of a Strand. Runners posted to the pool hold it, so a strand destroyed while one is
// queued in the pool is never touched after its destructor returns.
class StrandImpl : public std::enable_shared_from_this<StrandImpl> {
public:
    StrandImpl(std::shared_ptr<PoolImpl> pool, std::string name, int max_concurrency,
               ShutdownMode mode)
        : pool_(std::move(pool)), name_(std::move(name)), max_(max_concurrency), mode_(mode) {}

    bool post(Task task) {
        bool schedule = false;
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return false;
            }
            queue_.push_back(std::move(task));
            if (scheduled_ < max_) {
                ++scheduled_;
                schedule = true;
            }
        }
        if (schedule) {
            schedule_runner();
        }
        return true;
    }

    TimerHandle post_after(std::chrono::milliseconds delay, Task task) {
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return {};
            }
        }
        std::weak_ptr<StrandImpl> weak = weak_from_this();
        auto state = pool_->add_timer(delay, [&](const std::shared_ptr<TimerState>& timer) {
            // Due: queue on the strand; the timer counts as fired only when the strand runs it,
            // so a cancel before then still stops it.
            return Task([weak, timer, task = std::move(task)] {
                if (auto self = weak.lock()) {
                    self->post([timer, task] {
                        if (timer->fire()) {
                            task();
                        }
                    });
                }
            });
        });
        if (!state) {
            return {};
        }
        {
            std::lock_guard lock(mutex_);
            std::erase_if(timers_, [](const std::weak_ptr<TimerState>& timer) {
                const auto alive = timer.lock();
                return !alive || !alive->pending();
            });
            timers_.push_back(state);
        }
        return TimerHandle(std::move(state));
    }

    bool is_current() const { return tls_strand == this; }

    void shutdown(std::chrono::milliseconds budget, bool bounded) {
        const auto deadline = deadline_after(budget);
        const int own = is_current() ? 1 : 0;
        std::deque<Task> dropped;
        std::vector<std::weak_ptr<TimerState>> timers;
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            timers.swap(timers_);
            if (mode_ == ShutdownMode::CancelQueued || !bounded) {
                dropped.swap(queue_);
            }
        }
        for (auto& weak : timers) {
            if (auto timer = weak.lock()) {
                timer->cancel();
            }
        }
        dropped.clear();
        {
            std::unique_lock lock(mutex_);
            const auto done = [&] {
                return queue_.empty() && executing_ <= own;
            };
            if (bounded) {
                idle_.wait_until(lock, deadline, done);
            } else {
                idle_.wait(lock, done);
            }
            dropped.swap(queue_);
        }
    }

private:
    void schedule_runner() {
        if (!pool_->post([self = shared_from_this()] { self->run_one(); })) {
            std::lock_guard lock(mutex_);
            --scheduled_;
            idle_.notify_all();
        }
    }

    // One task per runner, then the runner goes to the back of the pool's queue, so a busy
    // strand does not starve the others on the pool.
    void run_one() {
        Task task;
        {
            std::lock_guard lock(mutex_);
            if (queue_.empty()) {
                --scheduled_;
                idle_.notify_all();
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
            ++executing_;
        }
        const StrandImpl* previous = tls_strand;
        tls_strand = this;
        run_task(task, name_);
        task = nullptr;
        tls_strand = previous;
        bool again = false;
        {
            std::lock_guard lock(mutex_);
            --executing_;
            if (queue_.empty()) {
                --scheduled_;
            } else {
                again = true;
            }
            idle_.notify_all();
        }
        if (again) {
            schedule_runner();
        }
    }

    const std::shared_ptr<PoolImpl> pool_;
    const std::string name_;
    const int max_;
    const ShutdownMode mode_;
    std::mutex mutex_;
    std::condition_variable idle_;
    std::deque<Task> queue_;
    std::vector<std::weak_ptr<TimerState>> timers_;
    int scheduled_ = 0; // runners posted to the pool or running
    int executing_ = 0; // tasks running now
    bool closed_ = false;
};

} // namespace detail

// --- ThreadPool -------------------------------------------------------------------------------

ThreadPool::ThreadPool(std::string name, int threads)
    : impl_(std::make_shared<detail::PoolImpl>(std::move(name))) {
    if (threads < 1) {
        throw std::invalid_argument("A thread pool needs at least one thread.");
    }
    impl_->start(threads);
}

ThreadPool::~ThreadPool() {
    impl_->shutdown(std::chrono::milliseconds(0), false);
}

void ThreadPool::post(Task task) {
    impl_->post(std::move(task));
}

TimerHandle ThreadPool::post_after(std::chrono::milliseconds delay, Task task) {
    auto state = impl_->add_timer(delay, [&](const std::shared_ptr<TimerState>& timer) {
        return Task([timer, task = std::move(task)] {
            if (timer->fire()) {
                task();
            }
        });
    });
    return state ? TimerHandle(std::move(state)) : TimerHandle();
}

bool ThreadPool::is_current() const {
    return impl_->is_current();
}

void ThreadPool::shutdown(std::chrono::milliseconds budget) {
    impl_->shutdown(budget, true);
}

// --- Strand -----------------------------------------------------------------------------------

Strand::Strand(ThreadPool& pool, std::string name, int max_concurrency, ShutdownMode mode) {
    if (max_concurrency < 1) {
        throw std::invalid_argument("A strand runs at least one task at a time.");
    }
    impl_ =
        std::make_shared<detail::StrandImpl>(pool.impl_, std::move(name), max_concurrency, mode);
}

Strand::~Strand() {
    impl_->shutdown(std::chrono::milliseconds(0), false);
}

void Strand::post(Task task) {
    impl_->post(std::move(task));
}

TimerHandle Strand::post_after(std::chrono::milliseconds delay, Task task) {
    return impl_->post_after(delay, std::move(task));
}

bool Strand::is_current() const {
    return impl_->is_current();
}

void Strand::shutdown(std::chrono::milliseconds budget) {
    impl_->shutdown(budget, true);
}

// --- The GUI thread ---------------------------------------------------------------------------

void mark_gui_thread() {
    g_gui_thread.store(std::this_thread::get_id());
}

bool is_gui_thread() {
    return g_gui_thread.load() == std::this_thread::get_id();
}

void assert_not_gui_thread() {
#ifndef NDEBUG
    if (is_gui_thread()) {
        kLog.critical("A blocking call was made on the GUI thread");
        log::flush(std::chrono::milliseconds(100));
        std::abort();
    }
#endif
}

} // namespace fv
