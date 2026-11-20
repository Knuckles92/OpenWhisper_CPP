#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/subscription.h"

class QWidget;

namespace fv {

class ActivityState;
class Clock;
class Executor;
class Paths;
class Strand;
class ThreadPool;
struct AppIdentity;

namespace backup {
class BackupCoordinator;
class BackupService;
} // namespace backup

// What the backup lifecycle reaches beyond the coordinator. Every reference must outlive it; an
// empty function takes the default named beside it.
struct BackupLifecyclePorts {
    const Paths* paths = nullptr;
    const AppIdentity* identity = nullptr;
    ActivityState* activity = nullptr;
    // ctx.ui(): the coordinator's UI actor.
    Executor* ui = nullptr;
    Clock* clock = nullptr;
    // The archive functions (backup::real_backup_service() when null); must outlive the lifecycle.
    backup::BackupService* service = nullptr;
    // The main window's central widget, disabled while a backup or restore holds the data; null
    // (or an empty function) while there is none.
    std::function<QWidget*()> centralWidget;
    // QApplication::exit(code): never vetoed by a window, as quit() is.
    std::function<void(int code)> exitApp;
    // Whether Settings is recording or transcribing a cleanup rule, read as a backup is
    // reserved (the rule_dictation_busy gate); empty reads the activity state alone.
    std::function<bool()> ruleDictationBusy;
};

// The backup and restore side of OpenWhisper's ApplicationController
// (services/application_controller.py:741-828, setup_backup and its hooks): it builds the app's one
// backup::BackupCoordinator with a worker strand of its own and the build's version, reserves the
// data for a create or restore through the activity gates (backup_refusal: startup, recovery,
// recording, transcription, meetings, the cleanup rule), disables the main window's central widget
// while the reservation holds, runs the work inside every registered pause (the client record sync
// and the host's record requests, both M9's), and once a restore or import is staged without error
// asks the application to exit with backup::restart_exit_code, so that main() relaunches only after
// every data writer has closed.
// Affinity: UI actor; worker contexts run on the coordinator's worker.
class BackupLifecycle {
public:
    using WorkerContext = std::function<void(const std::function<void()>& body)>;

    explicit BackupLifecycle(BackupLifecyclePorts ports);
    // shutdown(), then the worker strand drains.
    ~BackupLifecycle();
    BackupLifecycle(const BackupLifecycle&) = delete;
    BackupLifecycle& operator=(const BackupLifecycle&) = delete;

    // The coordinator, which the context hands out as AppContext::service<BackupCoordinator>().
    backup::BackupCoordinator& coordinator() const { return *coordinator_; }

    // _on_startup_database_ready's last step: the 60 s schedule starts.
    void start();
    // Quit (ShutdownStep::BackupCoordinator): marks the app as shutting down, so the reservation
    // the coordinator ends no longer gives the window back, then shuts the coordinator down.
    void shutdown();

    // _acquire_backup: the refusal, or nullopt once the data is reserved and the central widget
    // disabled.
    std::optional<std::string> acquire();
    // _release_backup: the reservation ends; the central widget gets back the state it had, unless
    // the app is shutting down.
    void release();
    // _backup_record_context: `body` inside every registered pause, the first registered outermost.
    // Each pause ends as `body` leaves, by return or by exception.
    void runInWorkerContext(const std::function<void()>& body) const;
    // A pause the backup worker enters around every create and restore (M9's record sync and host
    // requests register theirs). Register before the first backup starts.
    void addWorkerContext(WorkerContext context);

    // _on_restore_staged: a stage without error restarts the app (exit code 73), unless it is
    // shutting down.
    void onRestoreStaged(const std::string& stage, const std::string& error);

private:
    BackupLifecyclePorts ports_;
    bool shuttingDown_ = false;
    bool reserved_ = false;
    // Whether the central widget was enabled when the reservation disabled it.
    bool centralWasEnabled_ = false;
    std::vector<WorkerContext> contexts_;
    std::unique_ptr<ThreadPool> pool_;
    std::unique_ptr<Strand> worker_;
    std::unique_ptr<backup::BackupCoordinator> coordinator_;
    Subscription staged_;
};

} // namespace fv
