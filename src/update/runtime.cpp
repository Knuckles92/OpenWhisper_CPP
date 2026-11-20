#include "update/runtime.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <system_error>
#include <utility>

#include "base/log.h"
#include "base/platform_info.h"
#include "download/verified_files.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "update/leftovers.h"
#include "update/release_feed.h"
#include "update/schedule.h"

namespace fv::update {

namespace {

constexpr log::Logger kLog("services.runtime.updater");

// str(exc) of an unexpected failure, as Python would show it.
std::string exception_text(const std::exception& error) {
    if (const auto* system = dynamic_cast<const std::system_error*>(&error)) {
        return download::os_error_text(*system);
    }
    return error.what();
}

std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& name : names) {
        out += (out.empty() ? "" : ", ") + name;
    }
    return out;
}

// The download's sys.platform: the one given, else the check's, else this build's.
std::string runtime_platform(std::string platform, const CheckContext& context) {
    if (!platform.empty()) {
        return platform;
    }
    return context.platform.empty() ? std::string(python_platform()) : context.platform;
}

} // namespace

// What the worker tasks reach. `alive` and `owner` are read and written on the UI executor only,
// so a result posted there after the runtime is gone is dropped.
struct UpdaterRuntime::Shared {
    Shared(CheckContext check, std::filesystem::path updates, Executor& ui_executor,
           Clock& clock_ref, std::string platform_name)
        : context(std::move(check)), updates_dir(std::move(updates)), ui(ui_executor),
          clock(clock_ref), platform(std::move(platform_name)) {}

    CheckContext context;
    std::filesystem::path updates_dir;
    Executor& ui;
    Clock& clock;
    std::string platform;
    NativeUpdateActions native;
    std::atomic<std::uint64_t> attempt{0};
    std::mutex download_mutex;
    bool alive = true;
    UpdaterRuntime* owner = nullptr;

    DownloadContext download_context() const {
        return DownloadContext{context.client, context.feed, context.install, updates_dir,
                               platform};
    }
};

UpdaterRuntime::UpdaterRuntime(CheckContext context, std::filesystem::path updates_dir,
                               Executor& ui, Executor& jobs, Executor& components, Clock& clock,
                               std::string platform, NativeUpdateActions native)
    : shared_(std::make_shared<Shared>(std::move(context), std::move(updates_dir), ui, clock,
                                       runtime_platform(std::move(platform), context))),
      ui_(ui), jobs_(jobs), components_(components), cancel_(shared_->context.cancel) {
    shared_->native = std::move(native);
    shared_->owner = this;
    watch_ = shared_->context.settings.watch(
        {keys::update_beta_track_enabled.key(), keys::update_check_enabled.key(),
         keys::update_notify_enabled.key()},
        ui_, [this](const Json& changed) {
            for (const auto& key : changed) {
                if (key.is_string() &&
                    key.get_ref<const std::string&>() == keys::update_beta_track_enabled.key()) {
                    // A new track means a new answer: the next automatic check is due now.
                    release_track_changed(shared_->context.settings);
                }
            }
            schedule_check();
        });
}

UpdaterRuntime::~UpdaterRuntime() {
    shared_->alive = false;
    shared_->owner = nullptr;
    stop();
}

void UpdaterRuntime::start_timer(std::chrono::milliseconds delay) {
    armed_delay_ = delay;
    check_timer_ = ui_.post_after(delay, [this] {
        armed_delay_.reset();
        maybe_start_check();
    });
}

void UpdaterRuntime::schedule_check(bool launch) {
    if (launch) {
        launch_check_pending_ = true;
    }
    start_timer(kUpdateCheckDelay);
}

void UpdaterRuntime::request_check(bool manual) {
    jobs_.post([shared = shared_, manual] {
        std::optional<UpdateCheckResult> result;
        std::string error;
        try {
            result = check_for_update(shared->context, true);
        } catch (const AppUpdateError& failure) {
            // The expected, user-facing kind: offline, rate limited, no update source.
            kLog.warning("Update check failed: {}", failure.what());
            error = failure.what();
        } catch (const std::exception& failure) {
            kLog.error_with(failure, "Update check failed");
            error = "Could not check for updates.";
        }
        shared->ui.post([shared, result = std::move(result), error = std::move(error), manual] {
            if (shared->alive) {
                shared->owner->on_check_finished(result, error, manual);
            }
        });
    });
}

void UpdaterRuntime::maybe_start_check() {
    const auto wait = check_wait(launch_check_pending_
                                     ? std::optional<std::chrono::seconds>(std::chrono::seconds(0))
                                     : std::nullopt);
    if (wait && wait->count() == 0) {
        launch_check_pending_ = false;
        request_check(false);
    } else if (wait) {
        start_timer(*wait);
    }
}

std::optional<std::chrono::milliseconds>
UpdaterRuntime::check_wait(std::optional<std::chrono::seconds> interval) const {
    std::optional<double> wait_s;
    try {
        const auto& context = shared_->context;
        const auto settings = context.settings.snapshot();
        wait_s = seconds_until_auto_check(
            *settings, shared_->clock.utc(), interval.value_or(kUpdateCheckInterval),
            detect_channel(context.install), has_update_source(context.feed));
    } catch (const std::exception& error) {
        kLog.warning("Could not read the update-check schedule: {}; not scheduling an automatic "
                     "check",
                     error.what());
        return std::nullopt;
    }
    if (!wait_s) {
        return std::nullopt;
    }
    return std::chrono::milliseconds(static_cast<std::int64_t>(*wait_s * 1000));
}

void UpdaterRuntime::on_check_finished(std::optional<UpdateCheckResult> result, std::string error,
                                       bool manual) {
    // The check saved its time, success or failure; wait from there.
    if (const auto wait = check_wait()) {
        start_timer(std::max(*wait, kUpdateRecheckFloor));
    }
    check_finished.emit(result, error, manual);
}

void UpdaterRuntime::request_download(const UpdateCheckResult& result, bool force_setup) {
    // Never clear and reuse a prior source: a quick retry must not revive a canceled worker.
    cancel_.cancel();
    cancel_ = CancelSource(shared_->context.cancel);
    const auto attempt = ++attempt_;
    shared_->attempt.store(attempt);
    components_.post([shared = shared_, result, force_setup, token = cancel_.token(), attempt] {
        const auto is_current = [&] {
            return shared->attempt.load() == attempt;
        };
        const auto finish = [&](std::optional<Handoff> handoff, std::string error) {
            shared->ui.post(
                [shared, attempt, handoff = std::move(handoff), error = std::move(error)] {
                    if (shared->alive) {
                        shared->owner->on_download_finished(attempt, handoff, error);
                    }
                });
        };
        if (!result.release) {
            if (is_current()) {
                finish(std::nullopt, "No installer is available.");
            }
            return;
        }
        const download::ProgressCallback progress = [&](std::string_view phase, std::int64_t done,
                                                        std::int64_t total) {
            if (is_current() && !token.cancelled()) {
                shared->ui.post(
                    [shared, attempt, phase = std::string(phase),
                     done = static_cast<std::uint64_t>(std::max<std::int64_t>(done, 0)),
                     total = static_cast<std::uint64_t>(std::max<std::int64_t>(total, 0))] {
                        if (shared->alive) {
                            shared->owner->on_download_progress(attempt, phase, done, total);
                        }
                    });
            }
        };
        Handoff handoff;
        try {
            std::lock_guard lock(shared->download_mutex);
            handoff = apply_update(shared->download_context(), result, progress, token, force_setup,
                                   shared->native);
        } catch (const AppUpdateError& error) {
            // A user-facing reason, a cancel included.
            kLog.warning("Update download failed: {}", error.what());
            const std::string message = error.what();
            if (is_current()) {
                finish(std::nullopt, message.empty() ? "The download failed." : message);
            }
            return;
        } catch (const std::exception& error) {
            kLog.error_with(error, "Update download failed");
            const auto message = exception_text(error);
            if (is_current()) {
                finish(std::nullopt, message.empty() ? "The download failed." : message);
            }
            return;
        }
        if (token.cancelled() || !is_current()) {
            discard_prepared_result(handoff, shared->native);
            if (is_current()) {
                finish(std::nullopt, "The download was canceled.");
            }
            return;
        }
        finish(std::move(handoff), "");
    });
}

void UpdaterRuntime::cancel_download() {
    cancel_.cancel();
}

void UpdaterRuntime::discard_handoff(const Handoff& handoff) {
    components_.post(
        [shared = shared_, handoff] { discard_prepared_result(handoff, shared->native); });
}

void UpdaterRuntime::on_download_progress(std::uint64_t attempt, std::string phase,
                                          std::uint64_t done, std::uint64_t total) {
    if (attempt == attempt_ && !cancel_.cancelled()) {
        download_progress.emit(phase, done, total);
    }
}

void UpdaterRuntime::on_download_finished(std::uint64_t attempt, std::optional<Handoff> handoff,
                                          std::string error) {
    if (attempt != attempt_ || cancel_.cancelled()) {
        if (handoff) {
            discard_handoff(*handoff);
        }
        if (attempt == attempt_) {
            download_finished.emit(std::nullopt, "The update was canceled.");
        }
        return;
    }
    download_finished.emit(handoff, error);
}

void UpdaterRuntime::prune_leftovers() {
    components_.post([shared = shared_] {
        std::vector<std::string> removed;
        try {
            removed = prune_stale_downloads(shared->updates_dir, shared->clock.utc());
        } catch (const std::exception& error) {
            kLog.error_with(error, "Could not prune finished update downloads");
            return;
        }
        if (!removed.empty()) {
            kLog.info("Removed finished update downloads: {}", joined(removed));
        }
    });
}

void UpdaterRuntime::stop() {
    check_timer_.cancel();
    armed_delay_.reset();
    watch_.disconnect();
    cancel_.cancel();
}

std::optional<std::chrono::milliseconds> UpdaterRuntime::check_timer_delay() const {
    if (!check_timer_.active()) {
        return std::nullopt;
    }
    return armed_delay_;
}

} // namespace fv::update
