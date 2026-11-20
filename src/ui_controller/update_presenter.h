#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QString>

#include "base/subscription.h"
#include "ui_controller/update_handoff.h"
#include "ui_setup/app_update_dialog.h"
#include "ui_shell/system_tray.h"
#include "update/check.h"

namespace fv {

class ActivityState;
class Executor;
class MainWindow;
class SettingsStore;

namespace update {
class UpdaterRuntime;
}

// What the update presenter wires. `window`, `settings`, `activity` and `ui` are required and must
// outlive it; `runtime` is null where no updater exists (a composition without a network client),
// and then checks and downloads are not offered.
struct UpdatePresenterParts {
    MainWindow* window = nullptr;
    SettingsStore* settings = nullptr;
    ActivityState* activity = nullptr;
    // ctx.ui(): the runtime's signals are received here.
    Executor* ui = nullptr;
    update::UpdaterRuntime* runtime = nullptr;
    // The registration the About box names the install channel by; null: where the executable
    // runs from (installChannel()).
    const update::InstallProbe* probe = nullptr;
    // sys.platform the install channel is named for ("win32", "linux", "darwin"); empty:
    // this build's.
    std::string platform;
    // set_ambient_status: background news that must not replace a message.
    std::function<void(const QString&)> ambientStatus;
    // _on_settings_page_changed(kind) of the UI controller; empty: the notice is refreshed alone.
    std::function<void(std::string_view kind)> settingsChanged;
    // QDesktopServices::openUrl.
    std::function<void(const QString& url)> openUrl;
    // How the dialog is put up: application modal, as exec() is, without a nested event loop.
    std::function<void(AppUpdateDialog&)> showDialog;
    // Where "Open releases page" goes after a failed check (RELEASES_PAGE_URL); "" opens nothing.
    QString releasesPage;
    // QMessageBox::about with the About box's title and text.
    std::function<void(QWidget* parent, const QString& title, const QString& html)> aboutBox;
    // The hand-off's ports; the presenter fills dialog, parent, activity, lastResult, download,
    // cancelDownload and abandon.
    UpdateHandoffPorts handoff;
};

// The update side of OpenWhisper's UIController (ui_qt/ui_controller.py:2539-2615, 651-653 and
// the About box's update line at 2839-2879) and of ApplicationController's updater wiring
// (services/runtime/updater.py:125-140): a finished check keeps the title-bar notice and the
// tray's Update item current (only for update_available, and only while both checks and notices
// are on), a failed automatic check becomes ambient status, a manual check opens the update dialog
// (or raises the one that is open, whose result it leaves alone), and a failed check keeps the
// release found earlier. The notice, the tray item and Help > Check for Updates open the dialog;
// its download, cancel and setup requests go through UpdateHandoff.
// Affinity: GUI thread. The runtime's signals are connected on `ui`.
class UpdatePresenter : public QObject {
    Q_OBJECT
public:
    explicit UpdatePresenter(UpdatePresenterParts parts, QObject* parent = nullptr);
    ~UpdatePresenter() override;

    // The tray Bootstrap made after the features installed: its Update item.
    void attachTray(SystemTray& tray);

    // _on_check_finished's UI half: a failed automatic check as ambient status
    // (update_check_failure_status), then on_update_check_finished.
    void onCheckFinished(const std::optional<update::UpdateCheckResult>& result,
                         const std::string& error, bool manual);
    // _refresh_update_notice.
    void refreshNotice();
    // show_found_update: the dialog on the release the notice offers, or the open one raised.
    void showFoundUpdate();
    // show_app_update_dialog. Returns at once; the dialog's close is handled when it comes.
    void showUpdateDialog(const std::optional<update::UpdateCheckResult>& result,
                          const QString& error = {});
    // _on_check_for_updates_requested: a manual check.
    void requestCheck();

    // "Update available: 0.2.0", "Development build, newer than 0.1.0", "Development build",
    // "Up to date", or "" before any check succeeded.
    QString aboutStatusLine() const;
    // show_about_dialog.
    void showAbout();

    AppUpdateDialog* dialog() const { return dialog_.data(); }
    const std::optional<update::UpdateCheckResult>& lastResult() const { return last_; }
    UpdateHandoff& handoff() { return *handoff_; }

private:
    void onDialogFinished(AppUpdateDialog* dialog,
                          const std::optional<update::UpdateCheckResult>& result);
    void raiseDialog();

    UpdatePresenterParts parts_;
    std::unique_ptr<UpdateHandoff> handoff_;
    QPointer<SystemTray> tray_;
    QPointer<AppUpdateDialog> dialog_;
    std::optional<update::UpdateCheckResult> last_;
    std::vector<Subscription> links_;
};

} // namespace fv
