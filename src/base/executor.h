#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace fv {

using Task = std::function<void()>;

namespace detail {
class PoolImpl;
class StrandImpl;
} // namespace detail

// What a TimerHandle shares with the executor that runs its timer. Executors outside fv_base
// (QtUiExecutor) make one per post_after and call fire() right before running the task.
// Affinity: thread-safe.
class TimerState {
public:
    TimerState() = default;
    // on_cancel runs once, on the cancelling thread, when cancel() stops a pending timer: the
    // executor's chance to drop the task early. It must not call back into this state.
    explicit TimerState(std::function<void()> on_cancel);
    TimerState(const TimerState&) = delete;
    TimerState& operator=(const TimerState&) = delete;

    bool fire();   // pending -> fired; false when the timer was cancelled (do not run the task)
    bool cancel(); // pending -> cancelled; false when it had already fired or been cancelled
    bool pending() const;

private:
    std::atomic<int> state_{0};
    std::function<void()> on_cancel_;
};

// A pending post_after. RAII: destroying it cancels the timer. Move-only.
// Affinity: thread-safe.
class TimerHandle {
public:
    TimerHandle() = default; // no timer
    explicit TimerHandle(std::shared_ptr<TimerState> state);
    TimerHandle(TimerHandle&&) noexcept;
    TimerHandle& operator=(TimerHandle&&) noexcept;
    TimerHandle(const TimerHandle&) = delete;
    TimerHandle& operator=(const TimerHandle&) = delete;
    ~TimerHandle();

    void cancel();
    bool active() const; // pending: neither fired nor cancelled
    // Lets the timer run without this handle (QTimer.singleShot's fire and forget); the handle
    // then holds no timer. The executor's shutdown still cancels it.
    void detach();

private:
    std::shared_ptr<TimerState> state_;
};

// Where work runs. Every UI actor runs on ctx.ui(); blocking work runs on a pool strand.
// A task that throws is logged ("base.executor") and does not stop the executor.
// Affinity: thread-safe.
class Executor {
public:
    virtual ~Executor() = default;
    virtual void post(Task) = 0;
    virtual TimerHandle post_after(std::chrono::milliseconds, Task) = 0; // QTimer.singleShot
    virtual bool is_current() const = 0;
};

enum class ShutdownMode { CancelQueued, Drain };

// Named threads "<name>-N": the app's pool is "fv-pool", so "fv-pool-0" and on. Timers run on
// the pool's own threads (there is no timer thread).
// After shutdown() posts are dropped and post_after returns an inactive handle.
// Affinity: thread-safe.
class ThreadPool final : public Executor {
public:
    ThreadPool(std::string name, int threads);
    // Drops queued tasks and timers, then waits for the running tasks to return.
    ~ThreadPool() override;
    void post(Task) override;
    TimerHandle post_after(std::chrono::milliseconds, Task) override;
    bool is_current() const override;
    // Stops accepting work and cancels the timers; queued tasks keep running until the queue is
    // empty or the budget is spent, and what is still queued then is dropped. Threads still in
    // a task after the budget are left to finish it on their own.
    void shutdown(std::chrono::milliseconds budget);

private:
    friend class Strand;
    std::shared_ptr<detail::PoolImpl> impl_;
};

// A serial (or N-wide) queue on a pool. Python's executors map to strands on one pool: jobs (2
// wide, cancel queued), persistence (1, drain at quit), components, walkthrough-library, backup,
// meeting, agent (1 each), io (4).
// Tasks start in posting order; with max_concurrency 1 each ends before the next starts.
// Affinity: thread-safe.
class Strand final : public Executor {
public:
    Strand(ThreadPool&, std::string name, int max_concurrency, ShutdownMode);
    // Drops queued tasks and timers, then waits for the running tasks to return.
    ~Strand() override;
    void post(Task) override;
    TimerHandle post_after(std::chrono::milliseconds, Task) override;
    // True only on a thread that is running one of this strand's tasks.
    bool is_current() const override;
    // Stops accepting work and cancels the timers. CancelQueued drops the queued tasks; Drain
    // runs them until the budget is spent and drops the rest. Waits at most the budget for the
    // running tasks (not counting the caller's own, when called from one).
    void shutdown(std::chrono::milliseconds budget);

private:
    std::shared_ptr<detail::StrandImpl> impl_;
};

// Called once by the Qt bootstrap, on the GUI thread (QtUiExecutor's constructor).
void mark_gui_thread();
bool is_gui_thread();

// Every blocking API calls it; in debug builds it fails when called on the GUI thread.
void assert_not_gui_thread();

} // namespace fv
