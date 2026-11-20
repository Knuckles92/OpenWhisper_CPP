#include "ui_controller/update_handoff.h"

#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <QCoreApplication>
#include <QMessageBox>

#include "appcore/activity.h"
#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "base/log.h"
#include "process/detached.h"
#include "ui_setup/app_update_dialog.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.ui_controller"};

// How long the watchdog gives the log to reach its file before the hard exit.
constexpr std::chrono::milliseconds kLogFlushBudget{1000};

} // namespace

struct HandoffWatchdog::State {
    std::mutex mutex;
    std::condition_variable changed;
    bool canceled = false;
    bool running = true;
};

HandoffWatchdog::HandoffWatchdog(std::shared_ptr<State> state) : state_(std::move(state)) {}

HandoffWatchdog::~HandoffWatchdog() = default;

std::shared_ptr<HandoffWatchdog> HandoffWatchdog::arm(std::chrono::milliseconds grace,
                                                      std::function<void()> hardExit) {
    if (!hardExit) {
        hardExit = [] {
            std::_Exit(0);
        };
    }
    auto state = std::make_shared<State>();
    std::thread([state, grace, hardExit = std::move(hardExit)] {
        bool fire = false;
        {
            std::unique_lock lock(state->mutex);
            fire = !state->changed.wait_for(lock, grace, [&state] { return state->canceled; });
        }
        if (fire) {
            // Logging is best effort here; the hard exit below must still run.
            try {
                kLog.error("Still running {:.0f}s after handing off to the updater; exiting hard",
                           std::chrono::duration<double>(grace).count());
                log::flush(kLogFlushBudget);
            } catch (...) {
            }
            hardExit();
        }
        {
            const std::lock_guard lock(state->mutex);
            state->running = false;
        }
        state->changed.notify_all();
    }).detach();
    return std::shared_ptr<HandoffWatchdog>(new HandoffWatchdog(std::move(state)));
}

void HandoffWatchdog::cancel() {
    {
        const std::lock_guard lock(state_->mutex);
        state_->canceled = true;
    }
    state_->changed.notify_all();
}

bool HandoffWatchdog::running() const {
    const std::lock_guard lock(state_->mutex);
    return state_->running;
}

bool HandoffWatchdog::waitStopped(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(state_->mutex);
    return state_->changed.wait_for(lock, timeout, [this] { return !state_->running; });
}

UpdateHandoff::UpdateHandoff(UpdateHandoffPorts ports) : ports_(std::move(ports)) {
    if (!ports_.activity) {
        throw std::invalid_argument("UpdateHandoff needs the activity state.");
    }
    if (!ports_.startDetached) {
        ports_.startDetached = [](const std::filesystem::path& program,
                                  std::span<const std::string> args,
                                  const std::filesystem::path& cwd) {
            return start_detached(program, args, cwd);
        };
    }
    if (!ports_.armWatchdog) {
        ports_.armWatchdog = [] {
            HandoffWatchdog::arm();
        };
    }
    if (!ports_.exitApp) {
        ports_.exitApp = [](int code) {
            QCoreApplication::exit(code);
        };
    }
    if (!ports_.question) {
        ports_.question = [](QWidget* parent, const QString& title, const QString& text) {
            return QMessageBox::question(parent, title, text, QMessageBox::Yes | QMessageBox::No,
                                         QMessageBox::No) == QMessageBox::Yes;
        };
    }
    if (!ports_.warning) {
        ports_.warning = [](QWidget* parent, const QString& title, const QString& text) {
            QMessageBox::warning(parent, title, text);
        };
    }
}

std::optional<QString> UpdateHandoff::busyWork() const {
    const auto reasons = busy_reasons(ports_.activity());
    if (reasons.empty()) {
        return std::nullopt;
    }
    return qt::toQString(reasons.front());
}

// ui_controller.py:2641-2653.
bool UpdateHandoff::confirmUpdateWhileBusy() {
    const std::optional<QString> busy = busyWork();
    if (!busy) {
        return true;
    }
    const QString product = qt::toQString(identity().product);
    return ports_.question(ports_.parent ? ports_.parent() : nullptr,
                           QStringLiteral("Install update?"),
                           QStringLiteral("%1 is busy with %2. Install the update anyway? Unsaved "
                                          "work in that task may be lost.")
                               .arg(product, *busy));
}

// ui_controller.py:2655-2671.
void UpdateHandoff::requestDownload(const update::UpdateCheckResult& result, bool forceSetup) {
    if (!confirmUpdateWhileBusy()) {
        if (AppUpdateDialog* dialog = ports_.dialog ? ports_.dialog() : nullptr) {
            dialog->setError(QStringLiteral("The update was not started."));
        }
        return;
    }
    canceled_ = false;
    if (ports_.download) {
        ports_.download(result, forceSetup);
    }
}

void UpdateHandoff::requestCancel() {
    canceled_ = true;
    if (ports_.cancelDownload) {
        ports_.cancelDownload();
    }
}

void UpdateHandoff::onDownloadProgress(const QString& phase, std::uint64_t done,
                                       std::uint64_t total) {
    if (AppUpdateDialog* dialog = ports_.dialog ? ports_.dialog() : nullptr) {
        dialog->setProgress(phase, static_cast<qint64>(done), static_cast<qint64>(total));
    }
}

// _fail_update (140-145).
void UpdateHandoff::fail(const QString& message, bool offerSetup) {
    if (AppUpdateDialog* dialog = ports_.dialog ? ports_.dialog() : nullptr) {
        dialog->setError(message, offerSetup);
        return;
    }
    ports_.warning(ports_.parent ? ports_.parent() : nullptr, QStringLiteral("Update failed"),
                   message);
}

// _report_download_error (159-171), with _can_offer_setup: only a native-mode result whose release
// has a verified setup can fall back to it.
void UpdateHandoff::reportDownloadError(const QString& error) {
    const std::optional<update::UpdateCheckResult> last =
        ports_.lastResult ? ports_.lastResult() : std::nullopt;
    const bool offerSetup = last && last->apply_mode == update::ApplyMode::Native &&
                            last->release && last->release->setup_asset &&
                            last->release->setup_asset->sha256 &&
                            !last->release->setup_asset->sha256->empty();
    if (AppUpdateDialog* dialog = ports_.dialog ? ports_.dialog() : nullptr) {
        dialog->setError(error, offerSetup);
    } else if (canceled_) {
        // The user asked to stop and closed the dialog; a modal "Update failed" box for their own
        // cancel is just noise.
        kLog.info("Update stopped after cancel: {}", qt::toUtf8(error));
        if (ports_.status) {
            ports_.status(QStringLiteral("Update canceled"));
        }
    } else {
        ports_.warning(ports_.parent ? ports_.parent() : nullptr, QStringLiteral("Update failed"),
                       error);
    }
}

// _drop_download (174-178).
void UpdateHandoff::drop(const update::Handoff& handoff, const QString& message) {
    if (ports_.abandon) {
        ports_.abandon(handoff);
    }
    if (AppUpdateDialog* dialog = ports_.dialog ? ports_.dialog() : nullptr) {
        dialog->setError(message);
    }
}

// _setup_installer_command's mutex release and _start_detached: the setup must not wait for this
// process's app mutexes, and a setup that did not start gives them back.
bool UpdateHandoff::startSetup(const update::Handoff& handoff) {
    if (ports_.releaseForSetup) {
        ports_.releaseForSetup();
    }
    const auto started = ports_.startDetached(
        handoff.setup_exe, handoff.args,
        handoff.transaction_id.empty() ? std::filesystem::path{} : handoff.setup_exe.parent_path());
    if (!started) {
        kLog.error("Could not launch the update: {}", started.error());
        return false;
    }
    return true;
}

// ui_controller.py:2683-2714, the setup branch (the native helper is M10's, the disk image
// macOS's).
void UpdateHandoff::onDownloadFinished(const std::optional<update::Handoff>& handoff,
                                       const std::string& error) {
    if (!error.empty() || !handoff) {
        reportDownloadError(qt::toQString(error.empty() ? "The download failed." : error));
        return;
    }
    if (canceled_) {
        drop(*handoff, QStringLiteral("The update was canceled."));
        return;
    }
    if (!confirmUpdateWhileBusy()) {
        drop(*handoff, QStringLiteral("The update was not installed. Your work is still open."));
        return;
    }
    if (!startSetup(*handoff)) {
        // _report_launch_failure: acquire_application_mutex_or_exit, which here never exits (the
        // single-instance handshake owns that).
        if (ports_.reacquire) {
            ports_.reacquire();
        }
        // A native transaction that did not start is abandoned, and its prepared setup can
        // still be offered; a setup that did not start has nothing to fall back to.
        const bool native = !handoff->transaction_id.empty();
        if (native && ports_.abandon) {
            ports_.abandon(*handoff);
        }
        fail(native ? QStringLiteral("The updater could not be started.")
                    : QStringLiteral("The installer could not be started."),
             native);
        return;
    }
    exitForUpdate();
}

// exit_for_update (2750-2767): QApplication::quit() is only a request that the open update dialog
// (busy) and the main window (minimize to tray) both veto; exit() cannot be vetoed.
void UpdateHandoff::exitForUpdate() {
    if (AppUpdateDialog* dialog = ports_.dialog ? ports_.dialog() : nullptr) {
        dialog->markHandedOff();
    }
    ports_.armWatchdog();
    ports_.exitApp(0);
}

} // namespace fv
