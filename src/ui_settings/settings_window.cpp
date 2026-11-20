#include "ui_settings/settings_window.h"

#include <exception>
#include <utility>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/log.h"
#include "settings/store.h"
#include "speech/endpoints.h"
#include "ui_settings/settings_dialog.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.ui_controller"};

QString q(std::string_view text) {
    return qt::toQString(text);
}

// Picks the destination of an opening. A page whose create throws stays unbuilt and is logged;
// the window still opens, as the rail's own navigation shows such a page empty.
template <class Fn> void navigate(Fn&& fn) {
    try {
        fn();
    } catch (const std::exception& error) {
        kLog.error_with(error, "Couldn't show the Settings destination");
    } catch (...) {
        kLog.error("Couldn't show the Settings destination");
    }
}

} // namespace

SettingsWindow::SettingsWindow(AppContext& context, QObject* parent)
    : QObject(parent), context_(context) {
    raiseDialog = [](SettingsDialog& dialog) {
        dialog.show();
        dialog.raise();
        dialog.activateWindow();
    };
}

SettingsWindow::~SettingsWindow() = default;

SettingsDialog& SettingsWindow::ensureDialog() {
    if (dialog_ == nullptr) {
        dialog_ = new SettingsDialog(context_, parentWindow_);
        Q_EMIT dialogCreated(dialog_);
    }
    return *dialog_;
}

SettingsDialog* SettingsWindow::dialog() const {
    return dialog_;
}

bool SettingsWindow::isVisible() const {
    return dialog_ != nullptr && dialog_->isVisible();
}

SettingsDialog& SettingsWindow::prepared() {
    SettingsDialog& dialog = ensureDialog();
    try {
        dialog.refresh();
    } catch (const std::exception& error) {
        kLog.error_with(error, "Couldn't reload Settings");
    } catch (...) {
        kLog.error("Couldn't reload Settings");
    }
    return dialog;
}

void SettingsWindow::raise(SettingsDialog& dialog) {
    if (raiseDialog) {
        raiseDialog(dialog);
    }
}

void SettingsWindow::open(const QString& destination) {
    if (destination.isEmpty()) {
        SettingsDialog& dialog = prepared();
        navigate([&] { dialog.showHome(); });
        raise(dialog);
        return;
    }
    if (destination == QStringLiteral("engine_downloads")) {
        openDownloads(missingRuntime_ ? missingRuntime_() : QString());
        return;
    }
    SettingsDialog& dialog = prepared();
    navigate([&] {
        if (destination == QStringLiteral("text")) {
            dialog.focusCleanupModel();
        } else {
            dialog.selectDestination(destination);
        }
    });
    raise(dialog);
}

void SettingsWindow::openHfPolicy() {
    SettingsDialog& dialog = prepared();
    navigate([&] { dialog.focusHfPolicy(); });
    raise(dialog);
}

void SettingsWindow::openHotkeys() {
    SettingsDialog& dialog = prepared();
    navigate([&] { dialog.selectDestination(q(destinations::hotkeys)); });
    raise(dialog);
}

void SettingsWindow::openDownloads(const QString& componentId) {
    if (!componentId.isEmpty() && installShownElsewhere_ && installShownElsewhere_(componentId)) {
        // Started from guided setup, which shows this install itself.
        return;
    }
    SettingsDialog& dialog = prepared();
    navigate([&] { dialog.selectDestination(q(destinations::downloads)); });
    raise(dialog);
    if (!componentId.isEmpty()) {
        navigate([&] { dialog.showDownloads(QString(), componentId); });
    }
}

void SettingsWindow::setRemoteModelsOpener(std::function<bool(QWidget*)> open) {
    openRemoteModels_ = std::move(open);
}

void SettingsWindow::openEngineHelp(const QString& destination) {
    if (destination == QStringLiteral("remote_models")) {
        if (openRemoteModels_ && !openRemoteModels_(parentWindow_)) {
            open(q(destinations::remote_engine));
        }
        return;
    }
    if (destination == q(destinations::api_keys)) {
        SettingsDialog& dialog = prepared();
        // The selected server's key; one that needs none opens the page.
        QString env;
        try {
            env = q(speech::selected_endpoint(*context_.settings().snapshot()).api_key_env);
        } catch (const std::exception& error) {
            kLog.error_with(error, "Couldn't read the selected speech server");
        }
        navigate([&] { dialog.focusApiKeys(env); });
        raise(dialog);
        return;
    }
    if (destination == QStringLiteral("ondemand") || destination == q(destinations::downloads) ||
        destination == q(destinations::remote_engine)) {
        open(destination);
        return;
    }
    kLog.debug("No Settings destination for engine help '{}'", qt::toUtf8(destination));
}

void SettingsWindow::setMissingRuntimeProvider(std::function<QString()> provider) {
    missingRuntime_ = std::move(provider);
}

void SettingsWindow::setRuntimeInstallShownElsewhere(std::function<bool(const QString&)> shown) {
    installShownElsewhere_ = std::move(shown);
}

} // namespace fv
