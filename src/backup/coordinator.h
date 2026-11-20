#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "backup/backup.h"
#include "backup/create.h"
#include "backup/import.h"
#include "base/cancel.h"
#include "base/clock.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/json.h"
#include "base/paths.h"
#include "base/signal.h"

// services/backup_manager.py: the app's one backup worker and its opt-in schedule.
namespace fv::backup {

// The archive functions the coordinator runs on its worker (backup_manager.py's `service`);
// a test gives a fake. Blocking, called on the worker only.
class BackupService {
public:
    virtual ~BackupService() = default;
    virtual BackupInfo create_backup(const std::filesystem::path& destination,
                                     const CreateOptions& options) = 0;
    virtual BackupInfo inspect_backup(const std::filesystem::path& archive) = 0;
    // The path the coordinator reports through restore_staged: the stage folder.
    virtual std::filesystem::path prepare_restore(const std::filesystem::path& archive,
                                                  const std::filesystem::path& root) = 0;
    // Called on the UI actor by snapshot().
    virtual std::optional<std::filesystem::path>
    previous_data_dir(const std::filesystem::path& root) = 0;
    // An OpenWhisper data folder staged as a restore of `root` (import.h's
    // stage_from_data_root from legacy_identity() into identity(), which this does unless
    // overridden; not pure, so a service written before imports keeps compiling). Its stage is
    // what the coordinator reports through restore_staged.
    virtual ImportResult prepare_import(const std::filesystem::path& source,
                                        const std::filesystem::path& root,
                                        const ImportOptions& options);
};

// create.h's, inspect.h's and restore.h's functions under identity()'s names.
BackupService& real_backup_service();

// Create, inspect and stage-restore requests, one at a time, on a worker; automatic backups on a
// daily or weekly schedule checked every 60 s. Creating and restoring reserve the data through
// Hooks::acquire (inspection does not); restore is not cancellable, since its final journal is
// the restart contract.
// Affinity: UI actor (public methods and every signal but `progress` on `ui`); work runs on
// `worker` (a strand of one), which emits `progress`.
class BackupCoordinator {
public:
    struct Hooks {
        // Reserves the data for a create or restore: the refusal text, or nullopt to go ahead.
        // A thrown exception's what() is the refusal. Called on the UI actor.
        std::function<std::optional<std::string>()> acquire;
        // Ends that reservation, on the UI actor.
        std::function<void()> release;
        // Runs the create or restore inside whatever must pause meanwhile, on the worker.
        std::function<void(const std::function<void()>&)> worker_context;
    };

    // The data root is paths.data_root(), resolved. `service` defaults to
    // real_backup_service() and must outlive the coordinator.
    BackupCoordinator(const Paths& paths, const AppIdentity& app, Hooks hooks, Executor& ui,
                      Executor& worker, Clock& clock, std::string app_version,
                      BackupService* service = nullptr);
    // shutdown().
    ~BackupCoordinator();
    BackupCoordinator(const BackupCoordinator&) = delete;
    BackupCoordinator& operator=(const BackupCoordinator&) = delete;

    // Starts the 60 s schedule tick (not after shutdown).
    void start();
    // Stops the tick and refuses new work, cancels a backup, drops a request that has not
    // started, waits for a running one to return and ends any reservation. Idempotent.
    void shutdown();

    bool busy() const;
    // "backup", "inspect", "restore", or "" when idle.
    std::string operation() const;
    const std::filesystem::path& data_dir() const;

    // {last_backup, frequency, retention_count, destination, busy, operation, last_error,
    // previous_data_dir, next_backup_at}: next_backup_at in epoch seconds or null when off;
    // previous_data_dir "" when there is none or it cannot be read.
    Json snapshot() const;

    // "" when saved, else the refusal or the save error, as the Backup page shows it ("Wait for
    // the current backup operation to finish.", "Choose Off, Daily, or Weekly.", "Keep between 1
    // and 100 automatic backups.", "Choose an existing folder for automatic backups.", "Choose a
    // backup folder outside <product>'s recording folders.").
    std::string set_schedule(std::string_view frequency, int retention_count,
                             const std::filesystem::path& destination);

    void create_backup(const std::filesystem::path& destination, bool include_recordings = true);
    void inspect_backup(const std::filesystem::path& archive);
    void prepare_restore(const std::filesystem::path& archive);
    // Stages an OpenWhisper data folder as a restore, as prepare_restore stages a bundle: the
    // same reservation, operation "restore" and restore_staged (the stage, or the refusal, such
    // as OpenWhisperRunning's). The import is not cancellable either. With copy_credentials the
    // saved keys are copied once the stage is published.
    void prepare_import(const std::filesystem::path& source, bool copy_credentials);
    // Cancels a running create; anything else keeps going.
    void cancel();

    // _scheduled_tick: what the 60 s timer runs. Public for tests.
    void scheduled_tick();
    // The automatic-backup retry time in epoch seconds (600 s after an automatic failure).
    // Public for tests, as Python's tests set _retry_after.
    void set_retry_after(double seconds);

    Signal<std::string, int, int> progress; // phase, done 0..1000, total (1000, or 0 unknown)
    Signal<std::optional<BackupInfo>, std::string> backup_finished;     // info or error
    Signal<std::optional<BackupInfo>, std::string> inspection_finished; // info or error
    Signal<std::string, std::string> restore_staged; // stage path ("" on error), error
    Signal<> schedule_changed;
    Signal<bool> busy_changed;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace fv::backup
