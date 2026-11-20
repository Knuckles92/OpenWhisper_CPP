#pragma once

#include <mutex>
#include <utility>

#include "appkit/qt_bridge.h"

namespace fv::ui_walkthrough::detail {

// What a worker may touch of a widget: never the widget itself, which it reaches only through
// post(). The widget closes the mailbox in its destructor; a result arriving after that is
// dropped, and one already queued is dropped by post_task's QPointer.
// Affinity: thread-safe.
template <class Widget> class Mailbox {
public:
    explicit Mailbox(Widget* widget) : widget_(widget) {}

    // Runs fn(widget) on the widget's thread, queued, if the widget still lives then.
    template <class Fn> void post(Fn&& fn) {
        std::lock_guard lock(mutex_);
        if (widget_ == nullptr) {
            return;
        }
        // The widget cannot be destroyed while the lock is held, so post_task's QPointer is made
        // on a live object.
        qt::post(widget_, [target = widget_, fn = std::forward<Fn>(fn)]() mutable { fn(*target); });
    }

    void close() {
        std::lock_guard lock(mutex_);
        widget_ = nullptr;
    }

private:
    std::mutex mutex_;
    Widget* widget_;
};

} // namespace fv::ui_walkthrough::detail
