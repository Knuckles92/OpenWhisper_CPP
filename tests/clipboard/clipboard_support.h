#pragma once

// Shared helpers of the clipboard tests: the probe into TemporaryClipboard's state (what
// OpenWhisper's tests read as temporary._pending, _restore_timer, _prefetched), event-loop
// pumps, and builders for fakes.

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QTimer>

#include "base/clock.h"
#include "clipboard/temporary_clipboard.h"
#include "clipboard/testing/fake_backend.h"

namespace fv::clipboard {

struct TemporaryClipboardProbe {
    static bool pending(const TemporaryClipboard& clipboard) {
        return clipboard.pending_.has_value();
    }
    static bool restoreTimerActive(const TemporaryClipboard& clipboard) {
        return clipboard.restore_timer_->isActive();
    }
    static bool prefetchTimerActive(const TemporaryClipboard& clipboard) {
        return clipboard.prefetch_timer_->isActive();
    }
    static bool selecting(const TemporaryClipboard& clipboard) {
        return clipboard.selection_ != nullptr;
    }
    static const std::optional<PrefetchedSnapshot>&
    prefetched(const TemporaryClipboard& clipboard) {
        return clipboard.prefetched_;
    }
    static int captures(const TemporaryClipboard& clipboard) { return clipboard.captures_; }
    // The user's clipboard a lease stands for (Python's lease.snapshot), or null.
    static std::shared_ptr<const ClipboardSnapshot>
    leaseSnapshot(const TemporaryClipboard& clipboard, const focus::ClipboardLease& lease) {
        const auto found = clipboard.leases_.find(lease.id);
        return found == clipboard.leases_.end() ? nullptr : found->second.snapshot;
    }
    // The bytes the lease wrote under the marker format (Python's lease.token), or empty.
    static QByteArray leaseToken(const TemporaryClipboard& clipboard,
                                 const focus::ClipboardLease& lease) {
        const auto found = clipboard.leases_.find(lease.id);
        return found == clipboard.leases_.end() ? QByteArray() : found->second.token;
    }
    static void setClock(TemporaryClipboard& clipboard, const Clock& clock) {
        clipboard.clock_ = &clock;
    }

    // The queued hops and timers, run directly (the golden traces drive Python's slots so).
    static void startPrefetch(TemporaryClipboard& clipboard, std::chrono::milliseconds delay) {
        clipboard.startPrefetch(delay);
    }
    static void discardPrefetch(TemporaryClipboard& clipboard) { clipboard.discardPrefetchNow(); }
    static void firePrefetch(TemporaryClipboard& clipboard) {
        if (clipboard.prefetch_timer_->isActive()) {
            clipboard.prefetch_timer_->stop();
            clipboard.capturePrefetch();
        }
    }
    static void fireRestore(TemporaryClipboard& clipboard) {
        if (clipboard.restore_timer_->isActive()) {
            clipboard.restore_timer_->stop();
            clipboard.restorePending();
        }
    }
    static void poll(TemporaryClipboard& clipboard) {
        if (clipboard.selection_timer_->isActive()) {
            clipboard.pollSelection();
        }
    }
};

namespace testing {

// Runs the event loop for about ms milliseconds (OpenWhisper's _pump).
inline void pump(int ms = 30) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Processes events until predicate holds or timeout passes (OpenWhisper's _wait_for).
inline bool wait_for(const std::function<bool()>& predicate,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeout.count()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return predicate();
}

inline std::unique_ptr<FakeClipboardBackend> text_backend(const QString& text) {
    return std::make_unique<FakeClipboardBackend>(text);
}

// A TemporaryClipboard over backend, with the sequence source and renderer given (none by
// default, as offscreen) and other apps' copies visible.
inline std::unique_ptr<TemporaryClipboard> temporary_over(std::unique_ptr<ClipboardBackend> backend,
                                                          SequenceSource sequence = {},
                                                          Renderer renderer = {}) {
    return std::make_unique<TemporaryClipboard>(std::move(backend), std::move(sequence),
                                                std::move(renderer), nullptr, true);
}

// The fake's counter as a sequence source.
inline SequenceSource counter_of(const FakeClipboardBackend& backend) {
    return [&backend] {
        return backend.sequence();
    };
}

} // namespace testing
} // namespace fv::clipboard
