#include "appkit/qt_bridge.h"

#include <algorithm>
#include <exception>
#include <format>
#include <mutex>

#include <QByteArray>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include "base/log.h"

namespace fv::qt {

namespace {

constexpr log::Logger kLog{"appkit.qt"};

constexpr std::chrono::milliseconds kCoarseFrom{2000};

std::once_flag g_gui_thread_marked;

// Exceptions never reach Qt's event loop (architecture 4, rule 10).
void run_guarded(const Task& task, std::string_view where) noexcept {
    try {
        task();
    } catch (const std::exception& error) {
        try {
            kLog.error_with(error, std::format("A task run by {} raised an exception", where));
        } catch (...) {
        }
    } catch (...) {
        try {
            kLog.error("A task run by {} raised a non-standard exception", where);
        } catch (...) {
        }
    }
}

// The QTimer of one postAfter, touched only on the context's thread.
struct TimerSlot {
    QPointer<QTimer> timer;
};

} // namespace

namespace detail {

void post_task(QObject* context, Task task, std::string_view where) {
    if (context == nullptr) {
        return;
    }
    QPointer<QObject> guard(context);
    QMetaObject::invokeMethod(
        context,
        [guard, task = std::move(task), where = std::string(where)] {
            if (guard.isNull()) {
                return;
            }
            run_guarded(task, where);
        },
        Qt::QueuedConnection);
}

} // namespace detail

// A QPointer read on a worker races with the context's destruction on its own thread: the
// worker can see the object alive and then post to freed memory. So the context is published
// under a mutex that its destroyed() handler takes (on the context's thread, before the object
// goes); a worker that saw it alive finishes posting before the destructor goes on and removes
// the posted events. The lock is held only around a postEvent (architecture 4 rule 2).
struct QtUiExecutor::Target {
    std::mutex mutex;
    QObject* context = nullptr; // null once the context's destruction has begun

    // Runs post(context) under the lock when the context is still alive; false when it is not.
    template <class Post> bool with_context(Post&& post) {
        std::lock_guard lock(mutex);
        if (context == nullptr) {
            return false;
        }
        post(context);
        return true;
    }
};

QtUiExecutor::QtUiExecutor(QObject* context)
    : target_(std::make_shared<Target>()),
      thread_(context != nullptr ? context->thread() : nullptr) {
    std::call_once(g_gui_thread_marked, [] { mark_gui_thread(); });
    if (context != nullptr) {
        target_->context = context;
        // Direct: runs on the destroying thread, inside the context's destructor. It stays
        // connected after the executor goes, for the timers that still share the target.
        QObject::connect(context, &QObject::destroyed, [target = std::weak_ptr<Target>(target_)] {
            if (const auto locked = target.lock()) {
                std::lock_guard lock(locked->mutex);
                locked->context = nullptr;
            }
        });
    }
}

QtUiExecutor::~QtUiExecutor() = default;

void QtUiExecutor::post(Task task) {
    // No QPointer is made here: making one for an object whose destructor has begun asserts in
    // a debug Qt. Events posted to the context are removed when it goes, so the functor runs
    // only while it lives.
    target_->with_context([&](QObject* context) {
        QMetaObject::invokeMethod(
            context, [task = std::move(task)] { run_guarded(task, "the Qt UI executor"); },
            Qt::QueuedConnection);
    });
}

TimerHandle QtUiExecutor::post_after(std::chrono::milliseconds delay, Task task) {
    return postAfter(delay, defaultTimerType(delay), std::move(task));
}

TimerHandle QtUiExecutor::postAfter(std::chrono::milliseconds delay, Qt::TimerType type,
                                    Task task) {
    auto slot = std::make_shared<TimerSlot>();
    // Cancelling deletes the QTimer, and the task it holds, on the context's thread.
    auto state = std::make_shared<TimerState>([slot, target = target_] {
        const auto drop = [slot] {
            if (QTimer* timer = slot->timer.data()) {
                slot->timer.clear();
                delete timer;
            }
        };
        bool on_context_thread = false;
        target->with_context([&](QObject* owner) {
            on_context_thread = QThread::currentThread() == owner->thread();
            if (!on_context_thread) {
                QMetaObject::invokeMethod(owner, drop, Qt::QueuedConnection);
            }
        });
        // On its own thread the context cannot be destroyed during this call, so the timer is
        // deleted outside the lock.
        if (on_context_thread) {
            drop();
        }
    });
    const auto interval = std::max(delay, std::chrono::milliseconds(0));
    // The timer holds the state, so a detached handle's timer still runs.
    // Runs on the context's thread, where only the context's own destructor clears the target.
    auto arm = [slot, state, target = target_, interval, type, task = std::move(task)] {
        QObject* owner = nullptr;
        {
            std::lock_guard lock(target->mutex);
            owner = target->context;
        }
        if (owner == nullptr || !state->pending()) {
            return;
        }
        auto* timer = new QTimer(owner);
        timer->setSingleShot(true);
        timer->setTimerType(type);
        slot->timer = timer;
        QObject::connect(timer, &QTimer::timeout, timer, [timer, state, task] {
            timer->deleteLater();
            if (state->fire()) {
                run_guarded(task, "the Qt UI executor's timer");
            }
        });
        // Deleted with its context: the timer will never fire, so the handle stops being active.
        QObject::connect(timer, &QObject::destroyed, [state] { state->cancel(); });
        timer->start(interval);
    };
    if (QThread::currentThread() == thread_) {
        {
            std::lock_guard lock(target_->mutex);
            if (target_->context == nullptr) {
                return {};
            }
        }
        arm();
        return TimerHandle(std::move(state));
    }
    // A QTimer started on a thread without an event loop never fires (the reason for
    // OpenWhisper's qt_thread_callback), so it is started on the context's thread.
    const bool queued = target_->with_context([&](QObject* context) {
        QMetaObject::invokeMethod(context, std::move(arm), Qt::QueuedConnection);
    });
    if (!queued) {
        return {};
    }
    return TimerHandle(std::move(state));
}

bool QtUiExecutor::is_current() const {
    return thread_ != nullptr && QThread::currentThread() == thread_;
}

Qt::TimerType QtUiExecutor::defaultTimerType(std::chrono::milliseconds interval) {
    return interval >= kCoarseFrom ? Qt::CoarseTimer : Qt::PreciseTimer;
}

QString toQString(std::string_view utf8) {
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

std::string toUtf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

} // namespace fv::qt
