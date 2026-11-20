#include "ui_controller/update_presenter.h"

#include <stdexcept>
#include <utility>

#include <QDesktopServices>
#include <QMessageBox>
#include <QUrl>

#include "appcore/activity.h"
#include "appkit/qt_bridge.h"
#include "base/log.h"
#include "engine/version.h"
#include "settings/store.h"
#include "ui_controller/about_box.h"
#include "ui_setup/app_update_dialog.h"
#include "ui_shell/main_window.h"
#include "ui_shell/system_tray.h"
#include "update/runtime.h"
#include "update/schedule.h"
#include "update/trust.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.ui_controller"};

template <class T> T& required(T* value, const char* what) {
    if (value == nullptr) {
        throw std::invalid_argument(std::string("UpdatePresenter needs ") + what + ".");
    }
    return *value;
}

} // namespace

UpdatePresenter::UpdatePresenter(UpdatePresenterParts parts, QObject* parent)
    : QObject(parent), parts_(std::move(parts)) {
    MainWindow& window = required(parts_.window, "the main window");
    required(parts_.settings, "the settings");
    ActivityState& activity = required(parts_.activity, "the activity state");
    Executor& ui = required(parts_.ui, "the UI executor");
    if (!parts_.openUrl) {
        parts_.openUrl = [](const QString& url) {
            QDesktopServices::openUrl(QUrl(url));
        };
    }
    if (!parts_.showDialog) {
        parts_.showDialog = [](AppUpdateDialog& dialog) {
            dialog.setWindowModality(Qt::ApplicationModal);
            dialog.show();
        };
    }
    if (!parts_.aboutBox) {
        parts_.aboutBox = [](QWidget* owner, const QString& title, const QString& html) {
            QMessageBox::about(owner, title, html);
        };
    }

    UpdateHandoffPorts ports = std::move(parts_.handoff);
    ports.dialog = [this] {
        return dialog_.data();
    };
    QPointer<MainWindow> windowPtr = &window;
    ports.parent = [windowPtr]() -> QWidget* {
        return windowPtr.data();
    };
    ports.activity = [&activity] {
        return activity.snapshot();
    };
    ports.lastResult = [this] {
        return last_;
    };
    if (update::UpdaterRuntime* runtime = parts_.runtime) {
        ports.download = [runtime](const update::UpdateCheckResult& result, bool forceSetup) {
            runtime->request_download(result, forceSetup);
        };
        ports.cancelDownload = [runtime] {
            runtime->cancel_download();
        };
        ports.abandon = [runtime](const update::Handoff& handoff) {
            runtime->discard_handoff(handoff);
        };
    }
    handoff_ = std::make_unique<UpdateHandoff>(std::move(ports));

    connect(&window, &MainWindow::checkForUpdatesRequested, this, &UpdatePresenter::requestCheck);
    connect(&window, &MainWindow::updateNoticeRequested, this, &UpdatePresenter::showFoundUpdate);

    if (update::UpdaterRuntime* runtime = parts_.runtime) {
        links_.push_back(runtime->check_finished.connect(
            ui, [this](std::optional<update::UpdateCheckResult> result, std::string error,
                       bool manual) { onCheckFinished(result, error, manual); }));
        links_.push_back(runtime->download_progress.connect(
            ui, [this](std::string phase, std::uint64_t done, std::uint64_t total) {
                handoff_->onDownloadProgress(qt::toQString(phase), done, total);
            }));
        links_.push_back(runtime->download_finished.connect(
            ui, [this](std::optional<update::Handoff> handoff, std::string error) {
                handoff_->onDownloadFinished(handoff, error);
            }));
    }
}

UpdatePresenter::~UpdatePresenter() {
    links_.clear();
    if (dialog_ != nullptr) {
        // Its close must not reach a presenter that is gone.
        dialog_->disconnect(this);
        delete dialog_.data();
    }
}

void UpdatePresenter::attachTray(SystemTray& tray) {
    tray_ = &tray;
    // _on_tray_update (651-653).
    connect(&tray, &SystemTray::updateRequested, this, [this] {
        if (parts_.window != nullptr) {
            parts_.window->restoreFromTray();
        }
        showFoundUpdate();
    });
    refreshNotice();
}

// services/runtime/updater.py:125-140, after the runtime re-armed its timer.
void UpdatePresenter::onCheckFinished(const std::optional<update::UpdateCheckResult>& result,
                                      const std::string& error, bool manual) {
    if (!error.empty() && !manual && parts_.ambientStatus) {
        // Background news: offline, it used to land two seconds after "Model download failed"
        // and hide the reason the app can't work.
        parts_.ambientStatus(qt::toQString(update::update_check_failure_status(error)));
    }
    // on_update_check_finished (2544-2565).
    if (dialog_ != nullptr) {
        // The open dialog's download reports against the result it shows.
        if (manual) {
            raiseDialog();
        }
        return;
    }
    if (result) {
        last_ = result;
    }
    refreshNotice();
    if (manual) {
        showUpdateDialog(result, qt::toQString(error));
    }
}

// _refresh_update_notice (2567-2578).
void UpdatePresenter::refreshNotice() {
    QString version;
    if (last_ && last_->release) {
        const auto settings = parts_.settings->snapshot();
        if (update::shows_update_notice(last_->status, *settings)) {
            version = qt::toQString(last_->release->version);
        }
    }
    parts_.window->setUpdateNotice(version);
    if (tray_ != nullptr) {
        tray_->setUpdateAvailable(version);
    }
}

void UpdatePresenter::raiseDialog() {
    if (dialog_ != nullptr) {
        dialog_->raise();
        dialog_->activateWindow();
    }
}

// show_found_update (2580-2586).
void UpdatePresenter::showFoundUpdate() {
    if (dialog_ != nullptr) {
        raiseDialog();
        return;
    }
    showUpdateDialog(last_);
}

// show_app_update_dialog (2588-2614).
void UpdatePresenter::showUpdateDialog(const std::optional<update::UpdateCheckResult>& result,
                                       const QString& error) {
    auto* dialog = new AppUpdateDialog(result, error, *parts_.settings, parts_.runtime != nullptr,
                                       parts_.window);
    dialog_ = dialog;
    if (result) {
        const update::UpdateCheckResult shown = *result;
        connect(dialog, &AppUpdateDialog::downloadRequested, this,
                [this, shown] { handoff_->requestDownload(shown, false); });
        connect(dialog, &AppUpdateDialog::setupRequested, this,
                [this, shown] { handoff_->requestDownload(shown, true); });
    }
    connect(dialog, &AppUpdateDialog::cancelRequested, this, [this] { handoff_->requestCancel(); });
    connect(dialog, &QDialog::finished, this,
            [this, dialog, result](int) { onDialogFinished(dialog, result); });
    parts_.showDialog(*dialog);
}

// What follows dialog.exec() in show_app_update_dialog.
void UpdatePresenter::onDialogFinished(AppUpdateDialog* dialog,
                                       const std::optional<update::UpdateCheckResult>& result) {
    if (dialog->resultAction() == AppUpdateDialog::ResultAction::Primary) {
        if (result && result->release && !result->can_apply) {
            parts_.openUrl(qt::toQString(result->release->html_url));
        } else if (!result && !parts_.releasesPage.isEmpty()) {
            parts_.openUrl(parts_.releasesPage);
        }
    }
    // The dialog is closed: a download the user walked away from must not report into it.
    if (dialog_ == dialog) {
        dialog_ = nullptr;
    }
    dialog->deleteLater();
    // Its opt-out boxes save the same settings as Settings > Updates.
    if (parts_.settingsChanged) {
        parts_.settingsChanged("updates");
    } else {
        refreshNotice();
    }
}

void UpdatePresenter::requestCheck() {
    if (parts_.runtime == nullptr) {
        kLog.warning("No updater is running; the update check was not started");
        return;
    }
    parts_.runtime->request_check(true);
}

// show_about_dialog's status line (2840-2857).
QString UpdatePresenter::aboutStatusLine() const {
    if (!last_) {
        return {};
    }
    if (last_->status == update::UpdateStatus::UpdateAvailable && last_->release) {
        return QStringLiteral("Update available: %1").arg(qt::toQString(last_->release->version));
    }
    if (last_->status == update::UpdateStatus::Development) {
        const QString latest = last_->release ? qt::toQString(last_->release->version) : QString();
        return latest.isEmpty() ? QStringLiteral("Development build")
                                : QStringLiteral("Development build, newer than %1").arg(latest);
    }
    return QStringLiteral("Up to date");
}

void UpdatePresenter::showAbout() {
    const QString channel =
        parts_.probe != nullptr
            ? qt::toQString(parts_.platform.empty()
                                ? update::channel_label(update::detect_channel(*parts_.probe))
                                : update::channel_label(update::detect_channel(*parts_.probe),
                                                        parts_.platform))
            : installChannel();
    const AboutText about = aboutText(kVersion, channel, aboutStatusLine());
    parts_.aboutBox(parts_.window, about.title, about.html);
}

} // namespace fv
