#include "ui_setup/guided_setup_offer.h"

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QApplication>
#include <QEvent>
#include <QTimer>
#include <QWidget>

#include "appcore/guided_setup_state.h"
#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/log.h"
#include "models/component_runtime.h"
#include "models/model_runtime.h"
#include "recorder/recorder.h"
#include "settings/store.h"
#include "ui_settings/app_pages.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_window.h"
#include "ui_setup/openwhisper_import.h"
#include "ui_setup/setup_speech.h"
#include "ui_shell/bootstrap.h"
#include "ui_shell/main_window.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.ui_controller"};

namespace setup_state = appcore::guided_setup_state;

} // namespace

GuidedSetup::GuidedSetup(SettingsStore& store, std::function<GuidedSetupPorts()> ports,
                         QObject* parent)
    : QObject(parent), store_(store), ports_(std::move(ports)) {
    offerTimer_ = new QTimer(this);
    offerTimer_->setSingleShot(true);
    connect(offerTimer_, &QTimer::timeout, this, [this] { offerNow(); });
    hooks_.modalActive = [] {
        return QApplication::activeModalWidget() != nullptr;
    };
    hooks_.raiseDialog = [](GuidedSetupDialog& dialog) {
        dialog.show();
        dialog.raise();
        dialog.activateWindow();
    };
}

GuidedSetup::GuidedSetup(AppContext& context)
    : GuidedSetup(context.settings(), [&context] { return portsFor(context); }) {}

GuidedSetup::~GuidedSetup() {
    // The window is a child of the main window, which may outlive this service.
    delete dialog_.data();
}

void GuidedSetup::setMainWindow(QWidget* window) {
    mainWindow_ = window;
}

void GuidedSetup::watchFirstShow(QWidget* window) {
    watched_ = window;
    firstShowSeen_ = false;
    window->installEventFilter(this);
}

void GuidedSetup::offer() {
    if (setup_state::should_invite(store_)) {
        offerTimer_->start(kOfferDelayMs);
    }
}

void GuidedSetup::offerNow() {
    if (!setup_state::should_invite(store_) || (dialog_ != nullptr && dialog_->isVisible())) {
        return;
    }
    if (hooks_.modalActive() || mainWindow_ == nullptr || !mainWindow_->isVisible() ||
        mainWindow_->isMinimized()) {
        // A model-download consent, a recovery prompt, or a start in the tray.
        offerTimer_->start(kOfferRetryMs);
        return;
    }
    show(true);
}

void GuidedSetup::open() {
    show(false);
}

void GuidedSetup::show(bool welcome) {
    offerTimer_->stop();
    if (welcome && import_ != nullptr && !import_->imported()) {
        // The Welcome page offers the import once this finds OpenWhisper's data.
        import_->refresh();
    }
    if (dialog_ == nullptr) {
        GuidedSetupPorts ports = ports_();
        if (downloadModel_) {
            ports.downloadModel = downloadModel_;
        }
        if (providerSummary_) {
            ports.providerSummary = providerSummary_;
        }
        dialog_ = new GuidedSetupDialog(store_, std::move(ports), mainWindow_);
    }
    if (!dialog_->isVisible()) {
        dialog_->begin(welcome);
    }
    hooks_.raiseDialog(*dialog_);
}

void GuidedSetup::dictationReceived(const QString& text, bool /*pasted*/) {
    if (dialog_ != nullptr) {
        dialog_->dictationReceived(text);
    }
}

void GuidedSetup::dictationFailed(const QString& message) {
    if (dialog_ != nullptr) {
        dialog_->dictationFailed(message);
    }
}

bool GuidedSetup::showsDownload(const QString& model) const {
    // The app's started handler and this window's both hear model_download_started, in no fixed
    // order, so the download the window asked for counts before its own handler ran.
    return dialog_ != nullptr && dialog_->isVisible() && dialog_->claimsModelDownload(model);
}

bool GuidedSetup::showsRuntimeInstall(const QString& componentId) const {
    return dialog_ != nullptr && dialog_->showsRuntimeInstall(componentId);
}

void GuidedSetup::modelDownloadStarted(const QString& model) {
    if (dialog_ != nullptr) {
        dialog_->modelDownloadStarted(model);
    }
}

void GuidedSetup::modelDownloadProgress(const QString& model, std::int64_t done,
                                        std::int64_t total) {
    if (dialog_ != nullptr) {
        dialog_->modelDownloadProgress(model, done, total);
    }
}

void GuidedSetup::modelDownloadFinished(const QString& model, bool success) {
    if (dialog_ != nullptr) {
        dialog_->modelDownloadFinished(model, success);
    }
}

void GuidedSetup::runtimeInstallProgress(const QString& componentId, std::int64_t done,
                                         std::int64_t total) {
    if (dialog_ != nullptr) {
        dialog_->runtimeInstallProgress(componentId, done, total);
    }
}

void GuidedSetup::runtimeInstallFinished(const QString& componentId, bool success,
                                         const QString& message) {
    if (dialog_ != nullptr) {
        dialog_->runtimeInstallFinished(componentId, success, message);
    }
}

void GuidedSetup::setProviderSummary(std::function<ProviderSummary(const Json&)> summary) {
    providerSummary_ = summary;
    if (dialog_ != nullptr) {
        dialog_->setProviderSummary(std::move(summary));
    }
}

void GuidedSetup::setDownloadModel(std::function<void(const QString&)> download) {
    downloadModel_ = download;
    if (dialog_ != nullptr) {
        dialog_->setDownloadModel(std::move(download));
    }
}

void GuidedSetup::install(AppContext& context) {
    if (auto* window = context.find<MainWindow>()) {
        setMainWindow(window);
        connect(window, &MainWindow::guidedSetupRequested, this, [this] {
            try {
                open();
            } catch (const std::exception& error) {
                kLog.error_with(error, "Couldn't open guided setup");
            }
        });
        if (!context.find<UpdateHealthLaunch>())
            watchFirstShow(window);
    }
    if (auto* settings = context.find<SettingsWindow>()) {
        settings->setRuntimeInstallShownElsewhere(
            [this](const QString& componentId) { return showsRuntimeInstall(componentId); });
    }
    if (auto* import = context.find<OpenWhisperImport>()) {
        import_ = import;
        // The Welcome page's import row follows a discovery that finishes after it was built.
        connect(import, &OpenWhisperImport::availabilityChanged, this, [this] {
            if (dialog_ != nullptr) {
                dialog_->refreshImportRow();
            }
        });
    }
    auto* modelRuntime = context.find<models::ModelRuntime>();
    auto* components = context.find<models::ComponentRuntime>();
    if (modelRuntime == nullptr && components == nullptr) {
        return;
    }
    Executor& ui = context.ui();
    if (modelRuntime != nullptr) {
        setDownloadModel([modelRuntime](const QString& model) {
            modelRuntime->request_download(qt::toUtf8(model), false);
        });
        subscriptions_.push_back(modelRuntime->model_download_started.connect(
            ui, [this](const std::string& model) { modelDownloadStarted(qt::toQString(model)); }));
        subscriptions_.push_back(modelRuntime->model_download_progress.connect(
            ui, [this](const std::string& model, std::int64_t done, std::int64_t total) {
                modelDownloadProgress(qt::toQString(model), done, total);
            }));
        subscriptions_.push_back(modelRuntime->model_download_finished.connect(
            ui, [this](const std::string& model, bool success) {
                modelDownloadFinished(qt::toQString(model), success);
            }));
    }
    if (components != nullptr) {
        subscriptions_.push_back(components->component_progress.connect(
            ui, [this](const std::string& componentId, const std::string& /*phase*/,
                       std::int64_t done, std::int64_t total) {
                runtimeInstallProgress(qt::toQString(componentId), done, total);
            }));
        subscriptions_.push_back(components->component_install_finished.connect(
            ui, [this](const std::string& componentId, bool success, const std::string& message) {
                runtimeInstallFinished(qt::toQString(componentId), success, qt::toQString(message));
            }));
    }
}

bool GuidedSetup::eventFilter(QObject* watched, QEvent* event) {
    if (!firstShowSeen_ && watched == watched_ && event->type() == QEvent::Show) {
        firstShowSeen_ = true;
        watched->removeEventFilter(this);
        offer();
    }
    return QObject::eventFilter(watched, event);
}

GuidedSetupPorts GuidedSetup::portsFor(AppContext& context) {
    GuidedSetupPorts ports;
    SettingsWindow* window = context.find<SettingsWindow>();
    if (window != nullptr) {
        // _settings_for_guided_setup: the prepared window, refreshed.
        ports.settings = [window]() -> SettingsDialog& {
            SettingsDialog& dialog = window->ensureDialog();
            dialog.refresh();
            return dialog;
        };
        ports.openDestination = [window](const QString& destination) {
            window->open(destination);
        };
        ports.openAllSettings = [window] {
            // open_settings_dialog: Home, scrolled to the top.
            window->open();
        };
    }
    SettingsStore* store = &context.settings();
    if (auto* pages = context.find<AppPages>()) {
        const AppPagesHooks hooks = pages->resolvedHooks();
        if (hooks.previewRecorder) {
            ports.recorder = [recorder = hooks.previewRecorder, store] {
                return recorder(*store->snapshot());
            };
        }
    }
    ports.speechModelName = [](const Json& settings) {
        return setup_speech::speech_model_name(settings);
    };
    const Paths* paths = &context.paths();
    ports.speechModelDownloaded = [paths](const Json& settings) {
        return setup_speech::speech_model_downloaded(settings, *paths);
    };
    ports.cleanup = CleanupPort::settingsDefault();
    if (auto* import = context.find<OpenWhisperImport>()) {
        ports.importOffered = [import] {
            return import->offered();
        };
        ports.openImport = [import](QWidget* parent) {
            import->open(parent);
        };
    }
    return ports;
}

} // namespace fv
