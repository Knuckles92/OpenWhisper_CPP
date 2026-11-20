#include "backup/coordinator.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <format>
#include <mutex>
#include <system_error>
#include <utility>

#include "backup/detail/fs.h"
#include "backup/inspect.h"
#include "backup/preferences.h"
#include "backup/restore.h"
#include "base/ids.h"
#include "base/log.h"
#include "base/utf8.h"
#include "pyport/text.h"

namespace fv::backup {

namespace {

constexpr log::Logger logger("services.backup_manager");

constexpr std::chrono::milliseconds kTickInterval{60'000};
constexpr double kAutomaticRetrySeconds = 600;
// Same-phase progress closer together than this is dropped, unless it completes the phase.
constexpr std::chrono::milliseconds kProgressInterval{100};
// Qt's int signal is 32 bit and archives easily exceed it: progress goes out as 0..1000.
constexpr int kProgressScale = 1000;

class RealService final : public BackupService {
public:
    BackupInfo create_backup(const std::filesystem::path& destination,
                             const CreateOptions& options) override {
        return backup::create_backup(destination, options);
    }
    BackupInfo inspect_backup(const std::filesystem::path& archive) override {
        return backup::inspect_backup(archive);
    }
    std::filesystem::path prepare_restore(const std::filesystem::path& archive,
                                          const std::filesystem::path& root) override {
        return backup::prepare_restore(archive, root);
    }
    std::optional<std::filesystem::path>
    previous_data_dir(const std::filesystem::path& root) override {
        return backup::previous_data_dir(root);
    }
};

// Path.resolve().
std::filesystem::path resolve(const std::filesystem::path& path) {
    const std::filesystem::path absolute = detail::absolute_path(path);
    std::error_code error;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(absolute, error);
    return error ? absolute.lexically_normal() : resolved;
}

double epoch_seconds(std::chrono::system_clock::time_point at) {
    return std::chrono::duration<double>(at.time_since_epoch()).count();
}

// One request on the worker: started, or dropped by shutdown before it started.
struct Job {
    enum class State { Queued, Running, Done };
    std::mutex mutex;
    std::condition_variable changed;
    State state = State::Queued;
    bool dropped = false;
};

} // namespace

ImportResult BackupService::prepare_import(const std::filesystem::path& source,
                                           const std::filesystem::path& root,
                                           const ImportOptions& options) {
    return stage_from_data_root(legacy_identity(), source, identity(), root, options);
}

BackupService& real_backup_service() {
    static RealService service;
    return service;
}

struct BackupCoordinator::Impl {
    Impl(BackupCoordinator& owner, const Paths& paths, const AppIdentity& app, Hooks hooks,
         Executor& ui, Executor& worker, Clock& clock, std::string app_version,
         BackupService& service)
        : owner(&owner), data_dir(resolve(paths.data_root())), app(app), hooks(std::move(hooks)),
          ui(ui), worker(worker), clock(clock), app_version(std::move(app_version)),
          service(service) {}

    BackupCoordinator* owner; // valid while `alive`
    const std::filesystem::path data_dir;
    const AppIdentity& app;
    const Hooks hooks;
    Executor& ui;
    Executor& worker;
    Clock& clock;
    const std::string app_version;
    BackupService& service;

    // UI actor state.
    bool alive = true;
    bool busy = false;
    bool reserved = false;
    bool closed = false;
    std::string operation;
    std::string last_error;
    double retry_after = 0;
    TimerHandle timer;
    std::shared_ptr<Job> job;
    CancelSource cancel_source;

    // The worker records a finished backup while the UI actor may read them.
    mutable std::mutex preferences_mutex;
    BackupPreferences preferences;

    // Worker state: the progress throttle.
    std::chrono::steady_clock::time_point progress_at{};
    std::string progress_phase;

    double now() const { return epoch_seconds(clock.utc()); }

    std::vector<std::filesystem::path> data_folders() const {
        std::vector<std::filesystem::path> folders;
        for (const std::string_view name :
             {std::string_view("recordings"), std::string_view("meetings"),
              std::string_view("captures"), std::string_view("remote_records"), app.restore_dir}) {
            folders.push_back(data_dir / path_from_utf8(name));
        }
        return folders;
    }

    bool inside_data_folders(const std::filesystem::path& folder) const {
        for (const auto& data_folder : data_folders()) {
            if (detail::inside(folder, data_folder)) {
                return true;
            }
        }
        return false;
    }

    std::string outside_message() const {
        return std::format("Choose a backup folder outside {}'s recording folders.", app.product);
    }

    // _emit_result.
    void emit_result(std::string_view op, const std::optional<BackupInfo>& info,
                     const std::string& stage, const std::string& error) {
        if (op == "backup") {
            owner->backup_finished.emit(info, error);
        } else if (op == "inspect") {
            owner->inspection_finished.emit(info, error);
        } else {
            owner->restore_staged.emit(stage, error);
        }
    }

    // _claim: reserves the coordinator for one operation; refusals reach manual callers only.
    bool claim(std::string_view op, bool automatic) {
        if (busy || closed) {
            if (!automatic) {
                emit_result(op, std::nullopt, "", "Another backup operation is already running.");
            }
            return false;
        }
        if (op == "inspect") {
            return true;
        }
        std::optional<std::string> reason;
        try {
            if (hooks.acquire) {
                reason = hooks.acquire();
            }
        } catch (const std::exception& error) {
            logger.warning("Could not reserve the data for a {} operation", op);
            reason = error.what();
        }
        if (reason && !reason->empty()) {
            if (!automatic) {
                emit_result(op, std::nullopt, "", *reason);
            }
            return false;
        }
        reserved = true;
        return true;
    }

    // _report_progress, on the worker.
    void report_progress(std::string_view phase, std::uint64_t completed, std::uint64_t total) {
        const auto at = clock.steady();
        if (phase == progress_phase && completed != total && at - progress_at < kProgressInterval) {
            return;
        }
        progress_at = at;
        progress_phase = std::string(phase);
        const int percentage =
            total != 0 ? static_cast<int>(std::min<double>(
                             kProgressScale, static_cast<double>(completed) * kProgressScale /
                                                 static_cast<double>(total)))
                       : 0;
        owner->progress.emit(std::string(phase), percentage, total != 0 ? kProgressScale : 0);
    }

    // _remember_backup, on the worker: retention can touch a disconnected or slow drive, so it
    // stays with archive creation while schedule edits are locked out.
    // The file work runs on a copy outside preferences_mutex, so snapshot() on the UI actor never
    // waits for a slow destination drive. Only the UI actor changes the preferences otherwise, and
    // never while a request runs (set_schedule refuses while busy), so the copy is written back.
    std::string remember_backup(const BackupInfo& info, bool automatic) {
        BackupPreferences updated;
        {
            std::lock_guard lock(preferences_mutex);
            preferences.last_backup = backup_info_json(info);
            if (automatic) {
                preferences.last_auto_at = now();
                preferences.scheduled_archives.push_back(path_to_utf8(info.path));
            }
            updated = preferences;
        }
        std::string message;
        try {
            save_preferences(data_dir, updated);
            if (automatic) {
                // Leaves scheduled_archives as it was when an archive cannot be deleted.
                fv::backup::prune_automatic_backups(updated, app);
                save_preferences(data_dir, updated);
            }
        } catch (const std::exception& error) {
            message =
                std::string("Backup saved, but its history or retention could not be updated: ") +
                error.what();
            logger.warning("{}", message);
        }
        std::lock_guard lock(preferences_mutex);
        preferences = std::move(updated);
        return message;
    }

    // _finish, on the UI actor.
    void finish(const std::string& op, const std::optional<BackupInfo>& info,
                const std::string& stage, const std::string& error, bool automatic) {
        if (reserved) {
            if (hooks.release) {
                hooks.release();
            }
            reserved = false;
        }
        busy = false;
        operation.clear();
        last_error = error;
        if (automatic && !error.empty()) {
            retry_after = now() + kAutomaticRetrySeconds;
        }
        owner->busy_changed.emit(false);
        owner->schedule_changed.emit();
        if (!closed) {
            emit_result(op, info, stage, error);
        }
    }

    // `import` turns a "restore" into an import of the data folder at `path`.
    void start(const std::string& op, const std::filesystem::path& path, bool include_recordings,
               bool automatic, const std::shared_ptr<Impl>& self,
               std::optional<ImportOptions> import = std::nullopt) {
        if (!claim(op, automatic)) {
            return;
        }
        busy = true;
        operation = op;
        last_error.clear();
        cancel_source = CancelSource();
        owner->busy_changed.emit(true);

        auto current = std::make_shared<Job>();
        job = current;
        const CancelToken cancel = cancel_source.token();
        worker.post([self, current, op, path, include_recordings, automatic, cancel,
                     import = std::move(import)] {
            {
                std::lock_guard lock(current->mutex);
                if (current->dropped) {
                    current->state = Job::State::Done;
                    current->changed.notify_all();
                    self->post_finish(op, std::nullopt, "", "Backup operation failed.", automatic,
                                      self);
                    return;
                }
                current->state = Job::State::Running;
            }
            self->run(op, path, include_recordings, automatic, cancel, import);
            std::lock_guard lock(current->mutex);
            current->state = Job::State::Done;
            current->changed.notify_all();
        });
    }

    void post_finish(const std::string& op, std::optional<BackupInfo> info, std::string stage,
                     std::string error, bool automatic, const std::shared_ptr<Impl>& self) {
        ui.post([self, op, info = std::move(info), stage = std::move(stage),
                 error = std::move(error), automatic] {
            if (self->alive) {
                self->finish(op, info, stage, error, automatic);
            }
        });
    }

    // The work of one request, on the worker.
    void run(const std::string& op, const std::filesystem::path& path, bool include_recordings,
             bool automatic, const CancelToken& cancel,
             const std::optional<ImportOptions>& import) {
        std::optional<BackupInfo> info;
        std::string stage;
        std::string error;
        try {
            if (op == "inspect") {
                info = service.inspect_backup(path);
            } else {
                const auto body = [&] {
                    if (op == "backup") {
                        CreateOptions options;
                        options.data_root = data_dir;
                        options.include_recordings = include_recordings;
                        options.app_version = app_version;
                        options.progress = [this](std::string_view phase, std::uint64_t done,
                                                  std::uint64_t total) {
                            report_progress(phase, done, total);
                        };
                        options.cancel = cancel;
                        info = service.create_backup(path, options);
                    } else if (import) {
                        const ImportResult result = service.prepare_import(path, data_dir, *import);
                        for (const auto& warning : result.warnings) {
                            logger.warning("Import: {}", warning);
                        }
                        stage = path_to_utf8(result.stage);
                    } else {
                        stage = path_to_utf8(service.prepare_restore(path, data_dir));
                    }
                };
                if (hooks.worker_context) {
                    hooks.worker_context(body);
                } else {
                    body();
                }
                if (op == "backup") {
                    error = remember_backup(*info, automatic);
                }
            }
        } catch (const std::exception& failure) {
            logger.warning("Backup operation {} failed", op);
            info.reset();
            stage.clear();
            error = failure.what();
            if (error.empty()) {
                error = "Backup operation failed.";
            }
        } catch (...) {
            // Nothing leaves the worker raw, and the coordinator never stays busy for good.
            logger.warning("Backup operation {} failed", op);
            info.reset();
            stage.clear();
            error = "Backup operation failed.";
        }
        post_finish(op, std::move(info), std::move(stage), std::move(error), automatic,
                    self_ref.lock());
    }

    void arm_timer(const std::shared_ptr<Impl>& self) {
        std::weak_ptr<Impl> weak = self;
        timer = ui.post_after(kTickInterval, [weak] {
            auto impl = weak.lock();
            if (!impl || !impl->alive || impl->closed) {
                return;
            }
            impl->arm_timer(impl);
            impl->scheduled_tick(impl);
        });
    }

    // _scheduled_tick.
    void scheduled_tick(const std::shared_ptr<Impl>& self) {
        if (closed || busy || now() < retry_after) {
            return;
        }
        std::string destination;
        {
            std::lock_guard lock(preferences_mutex);
            const auto interval = schedule_interval(preferences.frequency);
            if (!interval ||
                now() - preferences.last_auto_seconds() < static_cast<double>(*interval)) {
                return;
            }
            destination = preferences.destination;
        }
        const std::filesystem::path folder_text = path_from_utf8(destination);
        if (destination.empty() || !folder_text.is_absolute() || !detail::is_dir(folder_text)) {
            last_error = "Automatic backup is waiting for its destination folder to be available.";
            owner->schedule_changed.emit();
            return;
        }
        const std::filesystem::path folder = resolve(folder_text);
        if (inside_data_folders(folder)) {
            last_error = outside_message();
            owner->schedule_changed.emit();
            return;
        }
        const std::filesystem::path archive =
            folder / path_from_utf8(auto_backup_name(app, clock.utc(), random_hex(12)));
        start("backup", archive, true, true, self);
    }

    std::weak_ptr<Impl> self_ref;
};

BackupCoordinator::BackupCoordinator(const Paths& paths, const AppIdentity& app, Hooks hooks,
                                     Executor& ui, Executor& worker, Clock& clock,
                                     std::string app_version, BackupService* service)
    : impl_(std::make_shared<Impl>(*this, paths, app, std::move(hooks), ui, worker, clock,
                                   std::move(app_version),
                                   service != nullptr ? *service : real_backup_service())) {
    impl_->self_ref = impl_;
    impl_->preferences = load_preferences(impl_->data_dir, impl_->now());
}

BackupCoordinator::~BackupCoordinator() {
    shutdown();
    impl_->alive = false;
}

void BackupCoordinator::start() {
    if (!impl_->closed) {
        impl_->arm_timer(impl_);
    }
}

void BackupCoordinator::shutdown() {
    Impl& impl = *impl_;
    impl.closed = true;
    impl.timer.cancel();
    impl.cancel_source.cancel();
    if (const auto current = impl.job) {
        std::unique_lock lock(current->mutex);
        if (current->state == Job::State::Queued) {
            current->dropped = true; // cancel_futures=True
        } else if (!impl.worker.is_current()) {
            current->changed.wait(lock, [&] { return current->state == Job::State::Done; });
        }
    }
    if (impl.reserved) {
        if (impl.hooks.release) {
            impl.hooks.release();
        }
        impl.reserved = false;
    }
}

bool BackupCoordinator::busy() const {
    return impl_->busy;
}

std::string BackupCoordinator::operation() const {
    return impl_->operation;
}

const std::filesystem::path& BackupCoordinator::data_dir() const {
    return impl_->data_dir;
}

Json BackupCoordinator::snapshot() const {
    const Impl& impl = *impl_;
    std::string previous_data_dir;
    try {
        const auto previous = impl.service.previous_data_dir(impl.data_dir);
        previous_data_dir = previous ? path_to_utf8(*previous) : "";
    } catch (const std::exception&) {
        logger.warning("Could not locate data retained before the last restore");
    }
    std::lock_guard lock(impl.preferences_mutex);
    const auto next = next_backup_at(impl.preferences, impl.now(), impl.retry_after);
    Json values = Json::object();
    values["last_backup"] = impl.preferences.last_backup;
    values["frequency"] = impl.preferences.frequency;
    values["retention_count"] = impl.preferences.retention_count;
    values["destination"] = impl.preferences.destination;
    values["busy"] = impl.busy;
    values["operation"] = impl.operation;
    values["last_error"] = impl.last_error;
    values["previous_data_dir"] = previous_data_dir;
    values["next_backup_at"] = next ? Json(*next) : Json(nullptr);
    return values;
}

std::string BackupCoordinator::set_schedule(std::string_view frequency, int retention_count,
                                            const std::filesystem::path& destination) {
    Impl& impl = *impl_;
    if (impl.busy) {
        return "Wait for the current backup operation to finish.";
    }
    if (frequency != "off" && frequency != "daily" && frequency != "weekly") {
        return "Choose Off, Daily, or Weekly.";
    }
    if (retention_count < 1 || retention_count > 100) {
        return "Keep between 1 and 100 automatic backups.";
    }
    std::string folder_text = py::strip(path_to_utf8(destination));
    if (frequency != "off") {
        const std::filesystem::path folder = resolve(path_from_utf8(folder_text));
        if (folder_text.empty() || !detail::is_dir(folder)) {
            return "Choose an existing folder for automatic backups.";
        }
        if (impl.inside_data_folders(folder)) {
            return impl.outside_message();
        }
        folder_text = path_to_utf8(folder);
    }
    {
        std::lock_guard lock(impl.preferences_mutex);
        BackupPreferences updated = impl.preferences;
        updated.frequency = std::string(frequency);
        updated.retention_count = retention_count;
        updated.destination = folder_text;
        try {
            save_preferences(impl.data_dir, updated);
        } catch (const std::exception& error) {
            return error.what();
        }
        impl.preferences = std::move(updated);
    }
    impl.last_error.clear();
    impl.retry_after = 0;
    schedule_changed.emit();
    return "";
}

void BackupCoordinator::create_backup(const std::filesystem::path& destination,
                                      bool include_recordings) {
    impl_->start("backup", destination, include_recordings, false, impl_);
}

void BackupCoordinator::inspect_backup(const std::filesystem::path& archive) {
    impl_->start("inspect", archive, true, false, impl_);
}

void BackupCoordinator::prepare_restore(const std::filesystem::path& archive) {
    impl_->start("restore", archive, true, false, impl_);
}

void BackupCoordinator::prepare_import(const std::filesystem::path& source, bool copy_credentials) {
    ImportOptions options;
    options.copy_credentials = copy_credentials;
    options.app_version = impl_->app_version;
    impl_->start("restore", source, true, false, impl_, std::move(options));
}

void BackupCoordinator::cancel() {
    // A staged restore must either finish publication or report failure: its final journal is
    // the restart contract, so it is not cancellable.
    if (impl_->operation == "backup") {
        impl_->cancel_source.cancel();
    }
}

void BackupCoordinator::scheduled_tick() {
    impl_->scheduled_tick(impl_);
}

void BackupCoordinator::set_retry_after(double seconds) {
    impl_->retry_after = seconds;
}

} // namespace fv::backup
