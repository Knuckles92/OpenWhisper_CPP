#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "hotkeys/capture.h"

namespace fv::hotkeys {

// Stands in for the global capture, which would hook the real keyboard (the _FakeCaptureThread
// of OpenWhisper's settings tests). start() and stop() only count; the test ends a started
// capture with capture(), cancel() or fail(), which emit on the calling thread as the capture
// thread would, then finished.
// The real capture also emits finished after stop(), later and from its own thread, so stop()
// here emits nothing: the test delivers that finished with finish_stopped() when it wants it,
// and code under test must not count on it arriving inside stop().
// Affinity: thread-safe.
class FakeCapture final : public HotkeyCapture {
public:
    void start() override {
        std::lock_guard lock(mutex_);
        ++starts_;
        running_ = true;
        stopped_ = false;
        stop_pending_ = false;
    }
    void stop() override {
        std::lock_guard lock(mutex_);
        stop_pending_ = stop_pending_ || running_;
        stopped_ = true;
        running_ = false;
    }
    // Ends a capture stopped while it ran the way the capture thread does: finished alone, once.
    // False, emitting nothing, when no such stop is waiting.
    bool finish_stopped() {
        {
            std::lock_guard lock(mutex_);
            if (!std::exchange(stop_pending_, false)) {
                return false;
            }
        }
        finished.emit();
        return true;
    }
    bool running() const override {
        std::lock_guard lock(mutex_);
        return running_;
    }
    bool wait(std::chrono::milliseconds) override { return true; }

    void capture(const std::string& hotkey) {
        if (end()) {
            captured.emit(hotkey);
            finished.emit();
        }
    }
    void cancel() {
        if (end()) {
            cancelled.emit();
            finished.emit();
        }
    }
    void fail(const std::string& message = std::string(kHotkeyCaptureFailureMessage)) {
        if (end()) {
            failed.emit(message);
            finished.emit();
        }
    }

    int starts() const {
        std::lock_guard lock(mutex_);
        return starts_;
    }
    bool stopped() const {
        std::lock_guard lock(mutex_);
        return stopped_;
    }

private:
    // A capture ends once; a stopped one ends silently.
    bool end() {
        std::lock_guard lock(mutex_);
        const bool was_running = running_;
        running_ = false;
        return was_running && !stopped_;
    }

    mutable std::mutex mutex_;
    int starts_ = 0;
    bool running_ = false;
    bool stopped_ = false;
    // A stop() ended a running capture and its finished has not been delivered yet.
    bool stop_pending_ = false;
};

// A CaptureFactory that makes FakeCaptures and keeps a pointer to each, newest last. The
// captures belong to whoever the factory hands them to; `made` is only valid while they live.
struct FakeCaptureFactory {
    std::vector<FakeCapture*> made;

    CaptureFactory factory() {
        return [this] {
            auto capture = std::make_unique<FakeCapture>();
            made.push_back(capture.get());
            return std::unique_ptr<HotkeyCapture>(std::move(capture));
        };
    }
    FakeCapture* last() const { return made.empty() ? nullptr : made.back(); }
};

} // namespace fv::hotkeys
