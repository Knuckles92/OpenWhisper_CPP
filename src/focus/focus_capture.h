#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "base/clock.h"
#include "base/executor.h"
#include "base/json.h"
#include "focus/capture_service.h"

namespace fv::focus {

// A read stuck this long abandons its thread (_capture.WEDGED_AFTER_S).
inline constexpr std::chrono::milliseconds WEDGED_AFTER{2000};
// Abandoned threads replaced before text capture goes off for the session.
inline constexpr int MAX_WORKER_REPLACEMENTS = 3;
// Deadline misses in a row before an app gets no more text reads this session.
inline constexpr int MAX_CONSECUTIVE_MISSES = 3;
// Apps remembered for recent_apps().
inline constexpr std::size_t RECENT_APPS = 12;

// Reads the text near the caret. Created, used and destroyed on one thread (it may own a COM
// apartment), the capture service's text thread.
// Affinity: owned thread "fv-focus".
class TextReader {
public:
    virtual ~TextReader() = default;
    // The focused element's text, or nullopt for unknown or another app. A password field gives
    // a blocked, empty context. May throw; the service logs the exception's type only.
    virtual std::optional<TextContext> read(const AppIdentity& identity, bool include_text,
                                            bool include_selection) = 0;
};

// What the capture service needs from the desktop.
// Affinity: identity() from any thread, one call at a time per caller; text_reader() on the
// text thread.
class FocusPlatform {
public:
    virtual ~FocusPlatform() = default;
    virtual std::string_view name() const = 0;
    // Windows names the focused app synchronously inside request() (native calls, well under a
    // millisecond); Linux asks on a short-lived thread, because Hyprland or X11 can block.
    virtual bool sync_identity() const = 0;
    virtual bool text_supported() const = 0;
    // The focused app, or nullopt during focus changes. May throw.
    virtual std::optional<AppIdentity> identity() = 0;
    // A reader for the calling thread. May throw: text capture then stays off this session.
    virtual std::unique_ptr<TextReader> text_reader() = 0;
};

// One finished or expired capture, as Python records it.
struct CaptureSample {
    double context_capture_ms = 0;
    int context_capture_timeouts = 0;

    bool operator==(const CaptureSample&) const = default;
};

struct FocusCaptureOptions {
    std::chrono::milliseconds deadline = CONTEXT_CAPTURE_DEADLINE;
    std::chrono::milliseconds wedged_after = WEDGED_AFTER;
    // The saved settings, read for app_context_excluded_apps before every read. A throwing
    // provider fails closed: nothing is read.
    std::function<std::shared_ptr<const Json>()> settings;
    // Where samples go; by default fv::metrics::record of both names.
    std::function<void(const CaptureSample&)> metrics;
    // Neither settings nor metrics is called once shutdown() (or the destructor) has returned,
    // although a thread abandoned in a read may outlive the service: what they refer to need
    // only outlive that call. Neither may call shutdown(): it waits for a call in flight.
    // Defaults: the system clock, and an own one-thread pool "fv-focus-deadline" for the
    // deadline timers. Both must outlive the service's threads when given.
    const Clock* clock = nullptr;
    Executor* timers = nullptr;
};

// The capture service: identity on the caller's thread, text on its own thread "fv-focus",
// because accessibility calls can hang on an unresponsive app:
// - every request resolves by the deadline, with the identity and whatever text was read by
//   then;
// - while a read is still running, new requests get identity only at once; they never queue
//   behind it;
// - a read stuck for wedged_after abandons its thread (the call cannot be cancelled; the thread
//   exits if it ever returns). The app it was reading gets no more text reads this session, and
//   a fresh thread takes over, at most MAX_WORKER_REPLACEMENTS times; after that text capture
//   stays off for the session;
// - an app that misses the deadline MAX_CONSECUTIVE_MISSES times in a row also gets no more text
//   reads this session.
// Python's services/focus_context/_capture.py CaptureService.
// Affinity: thread-safe.
class FocusCapture final : public CaptureService {
public:
    FocusCapture(std::shared_ptr<FocusPlatform> platform, FocusCaptureOptions options);
    // Shuts down; an abandoned thread still stuck in a call keeps what it uses alive.
    ~FocusCapture() override;
    FocusCapture(const FocusCapture&) = delete;
    FocusCapture& operator=(const FocusCapture&) = delete;

    std::shared_ptr<FocusRequest> request(bool include_text, bool include_selection) override;
    std::optional<AppIdentity> current_identity() override;
    std::optional<AppIdentity> identity_now(std::chrono::milliseconds timeout) override;
    void reread(const std::optional<AppIdentity>& identity,
                std::function<void(std::optional<TextContext>)> callback) override;
    void shutdown() override;
    std::vector<AppIdentity> recent_apps() override;

    // State for tests and diagnostics.
    struct State {
        bool has_worker = false;
        bool worker_busy = false;
        // Text threads started so far (the first one and every replacement).
        int workers_started = 0;
        int replacements = 0;
        bool text_off = false;
        std::set<std::string> disabled;
        std::map<std::string, int, std::less<>> misses;
    };
    State state() const;
    // Changes the deadline of later requests.
    void set_deadline(std::chrono::milliseconds deadline);

    struct Core;

private:
    std::unique_ptr<ThreadPool> own_timers_;
    std::shared_ptr<Core> core_;
};

} // namespace fv::focus
