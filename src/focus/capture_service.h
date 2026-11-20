#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "focus/snapshot.h"

namespace fv {
class SettingsStore;
}

namespace fv::focus {

// One capture's answer, Python's Future of a FocusSnapshot. It resolves exactly once, within the
// capture deadline, with whatever was known by then.
// Affinity: thread-safe. UI-thread callers only read it without waiting (try_get, ready).
class FocusRequest {
public:
    FocusRequest() = default;
    FocusRequest(const FocusRequest&) = delete;
    FocusRequest& operator=(const FocusRequest&) = delete;

    static std::shared_ptr<FocusRequest> resolved(FocusSnapshot snapshot);

    // Resolves the request unless it already is; true when this call did. `won` runs only when
    // this call resolves it, before anyone can see the snapshot, so a waiter woken by it also
    // sees what `won` recorded. Callbacks run on the settling thread, outside the lock.
    bool settle(FocusSnapshot snapshot, const std::function<void()>& won = {});
    bool ready() const;
    // The snapshot when resolved, without waiting.
    std::optional<FocusSnapshot> try_get() const;
    // Waits at most until the time; nullopt when it was not resolved by then. Never call it with
    // a time still ahead on the GUI thread.
    std::optional<FocusSnapshot> wait_until(std::chrono::steady_clock::time_point deadline) const;
    std::optional<FocusSnapshot> wait_for(std::chrono::milliseconds timeout) const;
    // Runs fn once with the snapshot: on the settling thread, or at once on this thread when the
    // request is already resolved. fn must not throw.
    void on_ready(std::function<void(const FocusSnapshot&)> fn);

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable resolved_;
    std::optional<FocusSnapshot> snapshot_;
    bool claimed_ = false; // a settle won and is publishing its snapshot
    std::vector<std::function<void(const FocusSnapshot&)>> callbacks_;
};

// Captures focus context off the caller's thread. Python's ContextCaptureService.
// Affinity: thread-safe; every method is safe from any thread and never throws.
class CaptureService {
public:
    virtual ~CaptureService() = default;

    // Starts a capture. include_text reads the text around the caret, include_selection the
    // selected text; a snapshot carries the selection only when asked.
    virtual std::shared_ptr<FocusRequest> request(bool include_text,
                                                  bool include_selection = false) = 0;
    // The focused app now, or nullopt where it is only known asynchronously.
    virtual std::optional<AppIdentity> current_identity() = 0;
    // The focused app now, waiting up to timeout where that is asynchronous (blocking: never on
    // the GUI thread there).
    virtual std::optional<AppIdentity> identity_now(std::chrono::milliseconds timeout) {
        (void)timeout;
        return current_identity();
    }
    // Reads identity's text again on the service thread, then calls back there. The callback
    // gets nullopt, possibly before this returns and on this thread, when the app no longer has
    // focus or its text cannot be read.
    virtual void reread(const std::optional<AppIdentity>& identity,
                        std::function<void(std::optional<TextContext>)> callback) = 0;
    virtual void shutdown() = 0;
    // Apps seen this session, newest first; kept in memory only.
    virtual std::vector<AppIdentity> recent_apps() { return {}; }
};

// Knows nothing: every snapshot is empty. Tests install this one.
// Affinity: thread-safe.
class NullCaptureService final : public CaptureService {
public:
    std::shared_ptr<FocusRequest> request(bool include_text, bool include_selection) override;
    std::optional<AppIdentity> current_identity() override;
    void reread(const std::optional<AppIdentity>& identity,
                std::function<void(std::optional<TextContext>)> callback) override;
    void shutdown() override;
};

// The service for this computer, reading the excluded apps from `settings` (which must outlive
// the service's shutdown() or destruction) and recording context_capture_ms and
// context_capture_timeouts through fv::metrics. A platform with no port (macOS, Linux) gets a
// NullCaptureService; so does one whose service cannot be made, with the warning "App awareness
// is unavailable on this computer".
std::shared_ptr<CaptureService> make_capture_service(const SettingsStore& settings);

// The process-wide service the composition root installs (Python's get_service and
// set_service). Until one is installed, and after set_service(nullptr), get_service() answers
// with a NullCaptureService: creating the real one needs the settings store.
std::shared_ptr<CaptureService> get_service();
void set_service(std::shared_ptr<CaptureService> service);
// Shuts the installed service down, if any, and uninstalls it.
void shutdown_service();
// Apps the installed service has seen this session, newest first.
std::vector<AppIdentity> recent_apps();
// Whether this computer can read text near the cursor at all.
bool text_reading_supported();

} // namespace fv::focus
