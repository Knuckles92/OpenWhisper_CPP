#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <QObject>
#include <QPointer>
#include <QString>

#include "base/executor.h"

class QThread;

namespace fv::qt {

// GUI-thread executor; post_after uses QTimer with the same Qt::TimerType as Python.
// The first one constructed marks its thread as the GUI thread (mark_gui_thread), which is what
// assert_not_gui_thread() checks. Tasks run on the context's thread; a task posted after the
// context is gone, or still queued when it goes, is dropped. A task that throws is logged with
// its type ("appkit.qt") and the event loop continues.
// Affinity: thread-safe; construct and destroy it on the context's thread. Workers may keep
// posting while the context is destroyed: the context's end is published under a lock, so a
// post never reaches a deleted object.
class QtUiExecutor final : public Executor {
public:
    explicit QtUiExecutor(QObject* context);
    ~QtUiExecutor() override;
    QtUiExecutor(const QtUiExecutor&) = delete;
    QtUiExecutor& operator=(const QtUiExecutor&) = delete;
    void post(Task) override;
    // QTimer.singleShot(ms, fn): the timer type Qt picks for that interval (defaultTimerType).
    TimerHandle post_after(std::chrono::milliseconds, Task) override;
    // A QTimer of the given type, for Python's QTimer instances (CoarseTimer unless set) and
    // singleShot calls that name a type. The timer is a child of the context, started on the
    // context's thread; destroying the returned handle (without detach()) cancels it.
    TimerHandle postAfter(std::chrono::milliseconds, Qt::TimerType, Task);
    bool is_current() const override;

    // Qt's rule for QTimer::singleShot without a type: PreciseTimer below 2 s, CoarseTimer from
    // 2 s on.
    static Qt::TimerType defaultTimerType(std::chrono::milliseconds);

private:
    // The context, shared with the timers this executor started.
    struct Target;
    std::shared_ptr<Target> target_;
    QThread* thread_;
};

namespace detail {
// Queues task to context's thread. At delivery it runs only if context still exists, and an
// exception it throws is logged with its type, never passed to Qt's event loop.
void post_task(QObject* context, Task task, std::string_view where);
} // namespace detail

// Runs fn on context's thread if context is still alive; otherwise drops it.
// Always queued, also from the context's own thread. fn may be move-only.
template <class Fn> void post(QObject* context, Fn&& fn) {
    auto shared = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
    detail::post_task(context, [shared] { (*shared)(); }, "qt::post");
}

// "Latest request wins": a result is delivered only if its token is still the newest.
// Python's relay-with-token classes: the GUI thread takes a token per request, the worker hands
// its result back with that token, and a result for an older request is dropped. The check is
// made again at delivery, so a request started while a result was queued still wins.
// Copies share one counter.
// Affinity: next() on the GUI thread; deliver() from any thread.
class TokenGate {
public:
    TokenGate() : latest_(std::make_shared<std::atomic<std::uint64_t>>(0)) {}

    std::uint64_t next() { return latest_->fetch_add(1) + 1; }
    // The newest token handed out; 0 before the first next().
    std::uint64_t current() const { return latest_->load(); }
    bool isCurrent(std::uint64_t token) const { return latest_->load() == token; }

    template <class Fn> void deliver(QObject* context, std::uint64_t token, Fn&& fn) {
        if (!isCurrent(token)) {
            return;
        }
        auto shared = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
        detail::post_task(
            context,
            [latest = latest_, token, shared] {
                if (latest->load() == token) {
                    (*shared)();
                }
            },
            "TokenGate::deliver");
    }

private:
    std::shared_ptr<std::atomic<std::uint64_t>> latest_;
};

QString toQString(std::string_view utf8);
std::string toUtf8(const QString&);

} // namespace fv::qt
