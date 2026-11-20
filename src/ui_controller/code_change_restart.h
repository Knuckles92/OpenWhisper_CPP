#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>

#include <QObject>
#include <QString>
#include <QTimer>

class QMessageBox;
class QWidget;

namespace fv {

class SettingsStore;

// CODE_CHANGE_POLL_MS: how often the app looks for its program replaced on disk.
inline constexpr std::chrono::milliseconds kCodeChangePoll{10'000};

// What the restart offer reaches. `settings` is required and must outlive it; an empty function
// takes the default named beside it.
struct CodeChangeRestartPorts {
    SettingsStore* settings = nullptr;
    // The main window: the box's parent.
    std::function<QWidget*()> parent;
    // _update_work_is_busy: the work a restart would stop, or nullopt.
    std::function<std::optional<QString>()> busyWork;
    // update::CodeIdentity::changed_since_launch.
    std::function<bool()> changed;
    // QMessageBox::exec of the offer (tests answer it).
    std::function<void(QMessageBox&)> execBox;
    // _confirm_quit_while_busy: true when nothing runs or the user agrees to stop it. Required.
    std::function<bool()> confirmQuit;
    // restart_app. Required.
    std::function<void()> restart;
};

// The restart offer after FioriVoce's program changed under the running app
// (ui_qt/ui_controller.py:2769-2830, A03-83, A16-30): an upgrade that replaced the installed
// executable leaves the running app mixing what it loaded with the new version's files. Every
// 10 s, while update_restart_prompt_enabled is on, the poll asks once the program changed and
// nothing is running; the offer is made once per run ("Later" means not again unprompted), its
// "Don't show this again" saves update_restart_prompt_enabled = false, and "Restart now" confirms
// running work first. A failure the change likely caused offers a restart again, with the error
// under Show Details and no mute box (M7's meeting error is the first such caller).
// Affinity: GUI thread.
class CodeChangeRestart : public QObject {
    Q_OBJECT
public:
    explicit CodeChangeRestart(CodeChangeRestartPorts ports, QObject* parent = nullptr);

    // The poll's timer starts (UIController's constructor does it).
    void start();
    void stop();
    bool polling() const { return timer_.isActive(); }

    // _on_code_change_poll.
    void poll();
    // offer_restart_for_code_change.
    void offer(const QString& failure = {});

private:
    CodeChangeRestartPorts ports_;
    QTimer timer_;
};

// restart_app (2820-2830): the geometry saved, then the application exits with
// backup::restart_exit_code (main() relaunches the app once every data writer has closed);
// exit() because windows can veto quit(). `saveGeometry` and `exitApp` (QCoreApplication::exit)
// may be empty.
void restartApp(const std::function<void()>& saveGeometry,
                const std::function<void(int)>& exitApp = {});

// show_apply_error_if_any (2832-2837): a leftover update error from the previous launch, read once
// and deleted (update::consume_apply_error), in an "Update failed" warning over `parent`.
// `warning` replaces QMessageBox::warning. Returns whether a box was shown.
bool showApplyErrorIfAny(
    const std::filesystem::path& updatesDir, QWidget* parent,
    const std::function<void(QWidget*, const QString& title, const QString& text)>& warning = {});

} // namespace fv
