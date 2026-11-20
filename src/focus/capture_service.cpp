#include "focus/capture_service.h"

#include "base/executor.h"
#include "base/log.h"
#include "focus/detail/platform.h"
#include "focus/focus_capture.h"
#include "settings/store.h"

namespace fv::focus {

namespace {

constexpr log::Logger kLog("services.focus_context");

std::mutex service_mutex;
std::shared_ptr<CaptureService> installed;

} // namespace

std::shared_ptr<FocusRequest> FocusRequest::resolved(FocusSnapshot snapshot) {
    auto request = std::make_shared<FocusRequest>();
    request->settle(std::move(snapshot));
    return request;
}

bool FocusRequest::settle(FocusSnapshot snapshot, const std::function<void()>& won) {
    {
        std::lock_guard lock(mutex_);
        if (snapshot_ || claimed_) {
            return false; // Already resolved (the deadline or the other path won).
        }
        claimed_ = true;
    }
    // Outside the lock: won may take the capture service's own.
    if (won) {
        won();
    }
    std::vector<std::function<void(const FocusSnapshot&)>> callbacks;
    {
        std::lock_guard lock(mutex_);
        snapshot_ = std::move(snapshot);
        callbacks.swap(callbacks_);
    }
    resolved_.notify_all();
    for (const auto& callback : callbacks) {
        callback(*snapshot_);
    }
    return true;
}

bool FocusRequest::ready() const {
    std::lock_guard lock(mutex_);
    return snapshot_.has_value();
}

std::optional<FocusSnapshot> FocusRequest::try_get() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

std::optional<FocusSnapshot>
FocusRequest::wait_until(std::chrono::steady_clock::time_point deadline) const {
    std::unique_lock lock(mutex_);
    if (!snapshot_ && std::chrono::steady_clock::now() < deadline) {
        assert_not_gui_thread();
        resolved_.wait_until(lock, deadline, [this] { return snapshot_.has_value(); });
    }
    return snapshot_;
}

std::optional<FocusSnapshot> FocusRequest::wait_for(std::chrono::milliseconds timeout) const {
    return wait_until(std::chrono::steady_clock::now() + timeout);
}

void FocusRequest::on_ready(std::function<void(const FocusSnapshot&)> fn) {
    std::optional<FocusSnapshot> now;
    {
        std::lock_guard lock(mutex_);
        if (!snapshot_) {
            callbacks_.push_back(std::move(fn));
            return;
        }
        now = snapshot_;
    }
    fn(*now);
}

std::shared_ptr<FocusRequest> NullCaptureService::request(bool, bool) {
    return FocusRequest::resolved(FocusSnapshot{});
}

std::optional<AppIdentity> NullCaptureService::current_identity() {
    return std::nullopt;
}

void NullCaptureService::reread(const std::optional<AppIdentity>&,
                                std::function<void(std::optional<TextContext>)> callback) {
    callback(std::nullopt);
}

void NullCaptureService::shutdown() {}

std::shared_ptr<CaptureService> make_capture_service(const SettingsStore& settings) {
    try {
        auto platform = detail::make_native_platform();
        if (!platform) {
            return std::make_shared<NullCaptureService>();
        }
        FocusCaptureOptions options;
        options.settings = [&settings] {
            return settings.snapshot();
        };
        return std::make_shared<FocusCapture>(std::move(platform), std::move(options));
    } catch (const std::exception&) {
        kLog.warning("App awareness is unavailable on this computer");
        return std::make_shared<NullCaptureService>();
    }
}

std::shared_ptr<CaptureService> get_service() {
    std::lock_guard lock(service_mutex);
    if (!installed) {
        static const auto null_service = std::make_shared<NullCaptureService>();
        return null_service;
    }
    return installed;
}

void set_service(std::shared_ptr<CaptureService> service) {
    std::lock_guard lock(service_mutex);
    installed = std::move(service);
}

void shutdown_service() {
    std::shared_ptr<CaptureService> service;
    {
        std::lock_guard lock(service_mutex);
        service = std::exchange(installed, nullptr);
    }
    if (service) {
        service->shutdown();
    }
}

std::vector<AppIdentity> recent_apps() {
    std::shared_ptr<CaptureService> service;
    {
        std::lock_guard lock(service_mutex);
        service = installed;
    }
    if (!service) {
        return {};
    }
    try {
        return service->recent_apps();
    } catch (const std::exception&) {
        kLog.debug("Recent apps unavailable");
        return {};
    }
}

bool text_reading_supported() {
    return detail::native_text_supported();
}

} // namespace fv::focus
