#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/executor.h"
#include "base/signal.h"
#include "base/subscription.h"
#include "update/apply.h"
#include "update/check.h"

// The app's updater (services/runtime/updater.py): scheduled release checks, verified downloads
// and their leftovers. Checks run on the jobs strand and downloads on the components strand, so
// a large setup never holds up transcription; results come back to the UI executor.
namespace fv::update {

// Least wait after a check before the next automatic one, whatever the stored times say: a copy
// that cannot save them must not ask again at once.
inline constexpr std::chrono::milliseconds kUpdateRecheckFloor{60 * 60 * 1000};
// The first check after a launch waits for startup to settle (config.UPDATE_CHECK_DELAY_MS).
inline constexpr std::chrono::milliseconds kUpdateCheckDelay{8000};

// Affinity: UI actor (every public method on `ui`); work runs on `jobs` and `components`.
class UpdaterRuntime {
public:
    // `context` (its client, feed, settings, probe and clock outlive the runtime) is what every
    // check uses; its cancel token is the app's root, which also ends a download in progress.
    // Downloads go to `updates_dir` (updates_root). `platform` is sys.platform's value; empty
    // means the context's, and when that is empty too, this build's.
    // Watches update_beta_track_enabled, update_check_enabled and update_notify_enabled: a
    // change of the track drops the last check time (release_track_changed), and any change
    // schedules the automatic check again, so turning checks back on checks once it is due.
    UpdaterRuntime(CheckContext context, std::filesystem::path updates_dir, Executor& ui,
                   Executor& jobs, Executor& components, Clock& clock, std::string platform = {},
                   NativeUpdateActions native = {});
    UpdaterRuntime(const UpdaterRuntime&) = delete;
    UpdaterRuntime& operator=(const UpdaterRuntime&) = delete;
    // stop(), and results still on their way are dropped.
    ~UpdaterRuntime();

    // Checks once startup is out of the way (kUpdateCheckDelay), or when it is next due. A
    // launch checks whatever the time since the last successful check (unless one failed within
    // the hour); the interval only spaces a running app's checks.
    void schedule_check(bool launch = false);
    // Runs a check on the jobs strand; check_finished reports it.
    void request_check(bool manual = true);

    // Downloads and verifies one attempt on the components strand. Every attempt has its own
    // cancel source: a quick retry never revives a canceled worker that has not unwound yet.
    void request_download(const UpdateCheckResult& result, bool force_setup = false);
    void cancel_download();
    // Gives up a hand-off the UI will not run (on the components strand).
    void discard_handoff(const Handoff& handoff);

    // Collects the downloads an earlier update left behind, on the components strand. The
    // native updater's transactions and setup backup are M10's.
    void prune_leftovers();

    // Stops the check timer, the settings watch and any download in progress.
    void stop();

    // (result or none, error, manual) after every check. The schedule has already been armed
    // from the saved times. The receiver shows a failed automatic check as ambient status
    // (update_check_failure_status) and hands the rest to the update UI.
    Signal<std::optional<UpdateCheckResult>, std::string, bool> check_finished;
    // (phase, done, total) of the current attempt, never after it was canceled.
    Signal<std::string, std::uint64_t, std::uint64_t> download_progress;
    // (hand-off or none, error) of the current attempt. A hand-off that arrives after a cancel
    // is discarded and reported as "The update was canceled."; a stale attempt reports nothing.
    Signal<std::optional<Handoff>, std::string> download_finished;

    // The delay the check timer was armed with, while it is pending.
    std::optional<std::chrono::milliseconds> check_timer_delay() const;

private:
    struct Shared;

    void start_timer(std::chrono::milliseconds delay);
    void maybe_start_check();
    // Milliseconds until the automatic check is due, nullopt when it is off (or the schedule
    // cannot be read, which is logged). `interval` replaces the usual wait since the last
    // successful check.
    std::optional<std::chrono::milliseconds>
    check_wait(std::optional<std::chrono::seconds> interval = std::nullopt) const;
    void on_check_finished(std::optional<UpdateCheckResult> result, std::string error, bool manual);
    void on_download_progress(std::uint64_t attempt, std::string phase, std::uint64_t done,
                              std::uint64_t total);
    void on_download_finished(std::uint64_t attempt, std::optional<Handoff> handoff,
                              std::string error);

    std::shared_ptr<Shared> shared_;
    Executor& ui_;
    Executor& jobs_;
    Executor& components_;
    TimerHandle check_timer_;
    std::optional<std::chrono::milliseconds> armed_delay_;
    bool launch_check_pending_ = false;
    CancelSource cancel_;
    std::uint64_t attempt_ = 0;
    Subscription watch_;
};

} // namespace fv::update
