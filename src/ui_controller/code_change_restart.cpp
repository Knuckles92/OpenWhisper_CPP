#include "ui_controller/code_change_restart.h"

#include <stdexcept>
#include <utility>

#include <QCheckBox>
#include <QCoreApplication>
#include <QMessageBox>
#include <QPushButton>

#include "appkit/qt_bridge.h"
#include "backup/restart.h"
#include "base/identity.h"
#include "base/json.h"
#include "base/log.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "update/apply_error.h"
#include "update/code_identity.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.ui_controller"};

} // namespace

CodeChangeRestart::CodeChangeRestart(CodeChangeRestartPorts ports, QObject* parent)
    : QObject(parent), ports_(std::move(ports)) {
    if (ports_.settings == nullptr || !ports_.confirmQuit || !ports_.restart) {
        throw std::invalid_argument(
            "CodeChangeRestart needs the settings, the quit confirmation and the restart.");
    }
    if (!ports_.changed) {
        ports_.changed = [] {
            return update::CodeIdentity::changed_since_launch();
        };
    }
    if (!ports_.execBox) {
        ports_.execBox = [](QMessageBox& box) {
            box.exec();
        };
    }
    timer_.setInterval(static_cast<int>(kCodeChangePoll.count()));
    connect(&timer_, &QTimer::timeout, this, &CodeChangeRestart::poll);
}

void CodeChangeRestart::start() {
    timer_.start();
}

void CodeChangeRestart::stop() {
    timer_.stop();
}

// _on_code_change_poll (2769-2776).
void CodeChangeRestart::poll() {
    bool enabled = true;
    try {
        enabled = keys::update_restart_prompt_enabled.read(*ports_.settings->snapshot());
    } catch (const std::exception& error) {
        kLog.warning("Could not read the restart prompt setting: {}", error.what());
    }
    if (!enabled) {
        timer_.stop();
        return;
    }
    if (ports_.changed() && !(ports_.busyWork && ports_.busyWork())) {
        offer();
    }
}

// offer_restart_for_code_change (2778-2818): asked unprompted once per run; after Later, only a
// failure asks again.
void CodeChangeRestart::offer(const QString& failure) {
    timer_.stop();
    const QString product = qt::toQString(identity().product);
    QMessageBox box(ports_.parent ? ports_.parent() : nullptr);
    box.setWindowTitle(QStringLiteral("Restart %1").arg(product));
    if (!failure.isEmpty()) {
        box.setIcon(QMessageBox::Warning);
        box.setText(QStringLiteral("%1 was updated while it was running, which likely caused this "
                                   "error.")
                        .arg(product));
        box.setInformativeText(QStringLiteral("Restart to finish the update, then try again."));
        box.setDetailedText(failure);
    } else {
        box.setIcon(QMessageBox::Information);
        box.setText(QStringLiteral("%1 was updated while it was running.").arg(product));
        box.setInformativeText(QStringLiteral("Restart to finish the update. Until you do, parts "
                                              "of the app that haven't loaded yet may not work."));
    }
    QCheckBox* mute = nullptr;
    if (failure.isEmpty()) {
        // Only the unprompted offer can be muted; an error still explains itself.
        mute = new QCheckBox(QStringLiteral("Don't show this again"));
        box.setCheckBox(mute);
    }
    QPushButton* restart = box.addButton(QStringLiteral("Restart now"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("Later"), QMessageBox::RejectRole);
    box.setDefaultButton(restart);
    ports_.execBox(box);
    if (mute != nullptr && mute->isChecked()) {
        ports_.settings->save_setting(keys::update_restart_prompt_enabled.key(), Json(false));
    }
    if (box.clickedButton() == restart && ports_.confirmQuit()) {
        ports_.restart();
    }
}

void restartApp(const std::function<void()>& saveGeometry,
                const std::function<void(int)>& exitApp) {
    kLog.info("Restarting {}", identity().product);
    if (saveGeometry) {
        saveGeometry();
    }
    if (exitApp) {
        exitApp(backup::restart_exit_code);
    } else {
        QCoreApplication::exit(backup::restart_exit_code);
    }
}

bool showApplyErrorIfAny(
    const std::filesystem::path& updatesDir, QWidget* parent,
    const std::function<void(QWidget*, const QString& title, const QString& text)>& warning) {
    const std::optional<std::string> message = update::consume_apply_error(updatesDir);
    if (!message || message->empty()) {
        return false;
    }
    if (warning) {
        warning(parent, QStringLiteral("Update failed"), qt::toQString(*message));
    } else {
        QMessageBox::warning(parent, QStringLiteral("Update failed"), qt::toQString(*message));
    }
    return true;
}

} // namespace fv
