#include "ui_controller/backup_lifecycle.h"

#include <chrono>
#include <stdexcept>
#include <utility>

#include <QCoreApplication>
#include <QWidget>

#include "appcore/activity.h"
#include "backup/coordinator.h"
#include "backup/restart.h"
#include "base/executor.h"
#include "base/log.h"
#include "engine/version.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"services.application_controller"};

// How long Quit waits for a backup that is still writing once its cancel was asked for.
constexpr std::chrono::milliseconds kWorkerShutdownBudget{5000};

template <class T> T& required(T* value, const char* what) {
    if (value == nullptr) {
        throw std::invalid_argument(std::string("BackupLifecycle needs ") + what + ".");
    }
    return *value;
}

void runNested(const std::vector<BackupLifecycle::WorkerContext>& contexts, std::size_t index,
               const std::function<void()>& body) {
    if (index == contexts.size()) {
        body();
        return;
    }
    contexts[index]([&contexts, index, &body] { runNested(contexts, index + 1, body); });
}

} // namespace

BackupLifecycle::BackupLifecycle(BackupLifecyclePorts ports) : ports_(std::move(ports)) {
    const Paths& paths = required(ports_.paths, "the paths");
    const AppIdentity& app = required(ports_.identity, "the identity");
    required(ports_.activity, "the activity state");
    Executor& ui = required(ports_.ui, "the UI executor");
    Clock& clock = required(ports_.clock, "the clock");
    if (!ports_.exitApp) {
        ports_.exitApp = [](int code) {
            QCoreApplication::exit(code);
        };
    }
    // The coordinator's worker: one backup at a time, never on a strand another feature waits on.
    pool_ = std::make_unique<ThreadPool>("fv-backup", 1);
    worker_ = std::make_unique<Strand>(*pool_, "backup", 1, ShutdownMode::CancelQueued);

    backup::BackupCoordinator::Hooks hooks;
    hooks.acquire = [this] {
        return acquire();
    };
    hooks.release = [this] {
        release();
    };
    hooks.worker_context = [this](const std::function<void()>& body) {
        runInWorkerContext(body);
    };
    // The manifest names the build that made the bundle, as Python's _version.__version__ does.
    coordinator_ = std::make_unique<backup::BackupCoordinator>(
        paths, app, std::move(hooks), ui, *worker_, clock, std::string(kVersion), ports_.service);
    staged_ = coordinator_->restore_staged.connect(
        ui, [this](std::string stage, std::string error) { onRestoreStaged(stage, error); });
}

BackupLifecycle::~BackupLifecycle() {
    staged_.disconnect();
    try {
        shutdown();
    } catch (const std::exception& error) {
        kLog.warning("Could not shut the backup coordinator down: {}", error.what());
    }
    worker_->shutdown(kWorkerShutdownBudget);
}

void BackupLifecycle::start() {
    coordinator_->start();
}

void BackupLifecycle::shutdown() {
    shuttingDown_ = true;
    coordinator_->shutdown();
}

// application_controller.py:757-783. The gate reads the activity every runtime keeps current;
// OpenWhisper held the capture, meeting and job locks for the same moment.
std::optional<std::string> BackupLifecycle::acquire() {
    ActivitySnapshot snapshot = ports_.activity->snapshot();
    if (shuttingDown_) {
        snapshot.shutting_down = true;
    }
    // _acquire_backup reads the Learned rules page's dictation_state at this moment.
    if (ports_.ruleDictationBusy && ports_.ruleDictationBusy()) {
        snapshot.rule_dictation_busy = true;
    }
    if (std::optional<std::string> refusal = backup_refusal(snapshot)) {
        return refusal;
    }
    ports_.activity->set_backup_in_progress(true);
    reserved_ = true;
    QWidget* central = ports_.centralWidget ? ports_.centralWidget() : nullptr;
    centralWasEnabled_ = central != nullptr && central->isEnabled();
    if (central != nullptr) {
        central->setEnabled(false);
    }
    return std::nullopt;
}

// application_controller.py:785-790.
void BackupLifecycle::release() {
    ports_.activity->set_backup_in_progress(false);
    const bool wasReserved = std::exchange(reserved_, false);
    if (shuttingDown_ || !wasReserved) {
        return;
    }
    if (QWidget* central = ports_.centralWidget ? ports_.centralWidget() : nullptr) {
        central->setEnabled(centralWasEnabled_);
    }
}

void BackupLifecycle::runInWorkerContext(const std::function<void()>& body) const {
    runNested(contexts_, 0, body);
}

void BackupLifecycle::addWorkerContext(WorkerContext context) {
    if (context) {
        contexts_.push_back(std::move(context));
    }
}

// application_controller.py:822-828: main() relaunches only after Bootstrap has closed every
// data writer.
void BackupLifecycle::onRestoreStaged(const std::string& /*stage*/, const std::string& error) {
    if (!error.empty() || shuttingDown_) {
        return;
    }
    ports_.exitApp(backup::restart_exit_code);
}

} // namespace fv
