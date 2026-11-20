#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <QString>

#include "update/apply.h"
#include "update/check.h"

class QWidget;

namespace fv {

class AppUpdateDialog;
struct ActivitySnapshot;

// HANDOFF_EXIT_GRACE_S: how long a process that handed off to the setup may take to leave before
// it is ended hard.
inline constexpr std::chrono::seconds kHandoffExitGrace{10};

// arm_handoff_watchdog (ui_controller.py:64-97): the setup waits for this exact process to leave
// (Inno's AppMutex and CloseApplications), so leaving must not depend on every window agreeing to
// close, on cleanup returning or on a thread joining. Once armed, a thread of its own logs "Still
// running 10s after handing off to the updater; exiting hard", flushes the log and calls
// `hardExit` (std::_Exit(0) by default) after `grace`, unless cancel() comes first. The thread is
// detached, so a normal exit never waits for it.
// Affinity: thread-safe.
class HandoffWatchdog {
public:
    static std::shared_ptr<HandoffWatchdog> arm(std::chrono::milliseconds grace = kHandoffExitGrace,
                                                std::function<void()> hardExit = {});
    ~HandoffWatchdog();
    HandoffWatchdog(const HandoffWatchdog&) = delete;
    HandoffWatchdog& operator=(const HandoffWatchdog&) = delete;

    // The exit came in time: the thread ends without firing.
    void cancel();
    // The thread is still waiting.
    bool running() const;
    // Waits up to `timeout` for the thread to end; true when it has.
    bool waitStopped(std::chrono::milliseconds timeout) const;

private:
    struct State;
    explicit HandoffWatchdog(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

// process/detached.h's start_detached.
using StartDetached = std::function<std::expected<std::uint32_t, std::string>(
    const std::filesystem::path& program, std::span<const std::string> args,
    const std::filesystem::path& cwd)>;

// What the hand-off reaches. An empty function takes the default named beside it; `activity` is
// required.
struct UpdateHandoffPorts {
    // The update dialog while it is open, else null.
    std::function<AppUpdateDialog*()> dialog;
    // The main window, the parent of the boxes.
    std::function<QWidget*()> parent;
    // What is running now (busy_reasons).
    std::function<ActivitySnapshot()> activity;
    // The result the notice offers (_last_update_result).
    std::function<std::optional<update::UpdateCheckResult>()> lastResult;
    // UpdaterRuntime::request_download, cancel_download and discard_handoff.
    std::function<void(const update::UpdateCheckResult&, bool forceSetup)> download;
    std::function<void()> cancelDownload;
    std::function<void(const update::Handoff&)> abandon;
    // The status line (set_status).
    std::function<void(const QString&)> status;
    // update::LaunchGate::release_for_setup and reacquire: the app mutexes main() holds. Empty: a
    // development build holds none.
    std::function<void()> releaseForSetup;
    std::function<bool()> reacquire;
    // fv::start_detached.
    StartDetached startDetached;
    // HandoffWatchdog::arm() with the 10 s grace.
    std::function<void()> armWatchdog;
    // QCoreApplication::exit(code).
    std::function<void(int code)> exitApp;
    // QMessageBox::question with Yes and No, No the default; true for Yes.
    std::function<bool(QWidget* parent, const QString& title, const QString& text)> question;
    // QMessageBox::warning.
    std::function<void(QWidget* parent, const QString& title, const QString& text)> warning;
};

// The UI half of applying an update (ui_controller.py:2616-2768 and the hand-off helpers at
// 130-239): the busy confirmation before a download starts and again when it finishes, the
// download, cancel and fallback requests of the update dialog, the progress it shows, and the
// hand-off itself: the verified setup is started detached with the app mutexes released
// immediately before (taken back if it does not start), then the app leaves with
// QCoreApplication::exit(0), which no window can veto as it can quit(), and the watchdog ends the
// process if it is still running 10 s later. The native updater's helper and the macOS disk image
// are not here: every hand-off before M10 is the setup's.
// Affinity: UI actor.
class UpdateHandoff {
public:
    explicit UpdateHandoff(UpdateHandoffPorts ports);

    // _update_work_is_busy: "a recording", "a meeting", "transcription", "a component install",
    // or nullopt.
    std::optional<QString> busyWork() const;
    // _confirm_update_while_busy: true when nothing runs or the user agrees to stop it.
    bool confirmUpdateWhileBusy();

    // _on_update_download_requested (forceSetup false) and _on_update_setup_requested (true).
    void requestDownload(const update::UpdateCheckResult& result, bool forceSetup = false);
    // _on_update_cancel_requested.
    void requestCancel();
    // on_update_download_progress.
    void onDownloadProgress(const QString& phase, std::uint64_t done, std::uint64_t total);
    // on_update_download_finished: shows the error, drops a download the user canceled or no
    // longer wants installed, or hands off to the setup.
    void onDownloadFinished(const std::optional<update::Handoff>& handoff,
                            const std::string& error);
    // exit_for_update.
    void exitForUpdate();

    bool canceled() const { return canceled_; }

private:
    void fail(const QString& message, bool offerSetup);
    void reportDownloadError(const QString& error);
    void drop(const update::Handoff& handoff, const QString& message);
    bool startSetup(const update::Handoff& handoff);

    UpdateHandoffPorts ports_;
    bool canceled_ = false;
};

} // namespace fv
