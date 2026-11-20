#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "appkit/feature.h"
#include "base/cancel.h"
#include "base/json.h"
#include "base/subscription.h"
#include "log/crash_handler.h"
#include "models/component_store.h"
#include "models/coordinator.h"
#include "models/engine_resolution.h"
#include "models/openwhisper_discovery.h"
#include "models/speech_model_cache.h"
#include "ui_controller/update_handoff.h"
#include "update/check.h"
#include "update/release_identity.h"

class QMessageBox;
class QString;
class QWidget;

namespace fv {

class AppUpdateDialog;
class ApplicationController;
class ActionWiring;
class BackupLifecycle;
class ClipboardService;
class Clock;
class CodeChangeRestart;
class EngineSetup;
class EnvReader;
class Executor;
class UpdatePresenter;
class HistoryManager;
class Paths;
class PersonalizeWiring;
class Recorder;
class SettingsBindings;
class SettingsStore;
class UiController;

namespace audio {
class Backend;
}
namespace clipboard {
class ClipboardBackend;
}
namespace focus {
class CaptureService;
class SyntheticKeys;
} // namespace focus
namespace hotkeys {
class KeyboardHook;
class MouseHookSource;
} // namespace hotkeys
namespace dictation {
class StreamingRuntime;
}
namespace models {
class ComponentRuntime;
class ModelRuntime;
} // namespace models
namespace ui {
class ThemeManager;
}
namespace http {
class Client;
}
namespace backup {
class BackupService;
}
namespace update {
class UpdaterRuntime;
}

// The download and install services' options, each left as it is for the real one: this
// computer, the built-in component catalog, the speech catalog's pinned files.
struct ModelServiceOptions {
    models::ComponentStoreOptions store;
    models::ComponentCoordinator::Options coordinator;
    models::SpeechModelCacheOptions cache;
    // HF_HUB_OFFLINE; nullptr: system_env(). Must outlive the services.
    const EnvReader* env = nullptr;
};

// What the controller builds that reaches the desktop, the disk outside the data folder or the
// crash log. Each empty function takes the real one; the end-to-end and wiring tests hand fakes
// (no test may install a keyboard or mouse hook, send keys or read the foreground window).
struct ControllerPlatform {
    // hotkeys::make_keyboard_hook().
    std::function<std::unique_ptr<hotkeys::KeyboardHook>()> keyboardHook;
    // hotkeys::make_mouse_hook_source(): the side buttons (the manager is built over the
    // keyboard hook above, so it hooks no mouse of its own).
    std::function<std::unique_ptr<hotkeys::MouseHookSource>()> mouseHook;
    // focus::make_capture_service(settings).
    std::function<std::shared_ptr<focus::CaptureService>(const SettingsStore&)> captureService;
    // focus::make_synthetic_keys().
    std::function<std::shared_ptr<focus::SyntheticKeys>()> syntheticKeys;
    // The application clipboard (QGuiApplication::clipboard()).
    std::function<std::unique_ptr<clipboard::ClipboardBackend>()> clipboardBackend;
    // Recorder::from_settings.
    std::function<std::unique_ptr<Recorder>(SettingsStore&, audio::Backend&, const Paths&,
                                            Executor& io)>
        recorder;
    // models::resolve_parakeet.
    std::function<models::ResolvedEngine(const Json& settings, const Paths&)> resolveEngine;
    // models::OpenWhisperAdopter(paths).adopt_missing.
    std::function<models::AdoptionReport(const Json& settings, const Paths&,
                                         const CancelToken& cancel)>
        adoptMissing;
    // ComponentStore(paths.components_root()) prune_orphans and settle_staged_changes.
    std::function<void(const Paths&, http::Client*)> recoverComponents;
    // log::CrashHandler::last_crash.
    std::function<std::optional<log::CrashInfo>(const Paths&)> lastCrash;
    // The "speech engine crashed" question box.
    std::function<bool(QWidget* parent)> offerCpuRuntime;
    // RuntimeSetup::ensureEngine of the setup feature.
    std::function<bool(QWidget* parent)> ensureEngine;
    // models::acquire_component for the loaded runtime.
    std::function<void(std::string_view componentId)> acquireComponent;
    // Adjusts the download and install services' options before they are built.
    std::function<void(ModelServiceOptions&)> modelServices;
    // ui::applyUiTheme over the application's palette; true when the palette changed.
    std::function<bool(const QString& preference)> applyTheme;
    // ui::applyUiFontScale (percent).
    std::function<void(int percent)> applyFontScale;

    // ---- updates, the setup hand-off, restarts and backups ----
    // update::probe_install(update::release_identity()): the registry, read once as the
    // controller is built.
    std::function<update::InstallProbe()> installProbe;
    // update::feed_config(system_env()): the release identity's source, or the loopback feed of
    // OPENWHISPER_CPP_UPDATE_FEED_URL.
    std::function<update::FeedConfig()> updateFeed;
    // sys.platform the update check, its downloads and the About box's install channel are
    // decided for ("win32"); empty: this build's. Tests of the Windows flow elsewhere set "win32".
    std::string updatePlatform;
    // The executor the updater arms its check timer on and reports to, and the clock it reads;
    // ctx.ui() and ctx.clock() when null. Both must outlive the controller.
    Executor* updaterUi = nullptr;
    Clock* updaterClock = nullptr;
    // fv::start_detached (the setup hand-off).
    StartDetached startDetached;
    // QCoreApplication::exit(code): 0 after the hand-off, 73 to restart.
    std::function<void(int code)> exitApp;
    // HandoffWatchdog::arm(): the hard exit 10 s after a hand-off.
    std::function<void()> armWatchdog;
    // QDesktopServices::openUrl (release notes, the releases page).
    std::function<void(const QString& url)> openUrl;
    // How the update dialog is put up (application modal, without a nested event loop).
    std::function<void(AppUpdateDialog&)> showUpdateDialog;
    // The update flow's QMessageBox::question (Yes and No, No the default) and the
    // QMessageBox::warning of a failed update and of the previous launch's apply error.
    std::function<bool(QWidget* parent, const QString& title, const QString& text)> question;
    std::function<void(QWidget* parent, const QString& title, const QString& text)> warning;
    // update::CodeIdentity::changed_since_launch, and the restart offer's QMessageBox::exec.
    std::function<bool()> codeChanged;
    std::function<void(QMessageBox&)> execRestartOffer;
    // QMessageBox::about of Help > About.
    std::function<void(QWidget* parent, const QString& title, const QString& html)> aboutBox;
    // The backup archive functions (backup::real_backup_service() when null); must outlive the
    // controller.
    backup::BackupService* backupService = nullptr;

    // ---- text models ----
    // The environment API keys are read from beside the credential store and the .env file
    // (system_env() when null); must outlive the controller. Tests give one with no keys.
    const EnvReader* keyEnvironment = nullptr;
};

// The controller feature (architecture 8.5): it builds every service in OpenWhisper's
// construction order (settings seeded first, the recorder from the settings, the clipboard, the
// focus capture and synthetic keys, History and its one StatsRepository, the transcription
// runtime, the application controller, the hotkeys, the live preview, then, with a network
// client, the model and component runtimes over one shared inventory and component coordinator
// and the EngineControl they ask) and provides them; install() wires Quick Record, Upload File,
// the compact controller, the overlay, the tray, the History panel, the main window, the
// Settings pages, Downloads, Home and the guided setup, and hands Bootstrap its lifecycle (the
// tray once made, the first paint, the ambient status). It also provides the backup coordinator
// (BackupLifecycle) and the updater (update::UpdaterRuntime, with a network client), and wires the
// update notice, dialog and hand-off, the code-change restart offer and the previous launch's
// update error.
// Affinity: UI actor.
class ControllerFeature final : public Feature {
public:
    static constexpr std::string_view kId = "controller";

    ControllerFeature();
    explicit ControllerFeature(ControllerPlatform platform);
    ~ControllerFeature() override;

    std::string_view id() const override { return kId; }
    std::vector<std::string_view> required_features() const override {
        return {"history", "overlay", "record", "setup", "shell"};
    }
    void provide(AppContext& context) override;
    void install(AppContext& context) override;

    UiController* uiController() const { return ui_.get(); }
    ClipboardService* clipboardService() const { return clipboard_.get(); }
    ApplicationController* applicationController() const { return app_; }
    EngineSetup* engineSetup() const { return engineSetup_.get(); }
    SettingsBindings* settingsBindings() const { return bindings_.get(); }
    BackupLifecycle* backupLifecycle() const { return backup_.get(); }
    UpdatePresenter* updatePresenter() const { return updates_.get(); }
    CodeChangeRestart* codeChangeRestart() const { return codeChange_.get(); }
    // The M4 text features' share of the controller (AI cleanup, dictionary learning, Command
    // Mode and transforms, rule dictation, the profile and cleanup callbacks).
    PersonalizeWiring* personalizeWiring() const { return personalize_.get(); }
    // The updater; null without a network client.
    update::UpdaterRuntime* updater() const;

private:
    struct UpdateServices;

    // The Settings bindings, Downloads' and Home's sources, the guided setup's share of the
    // downloads, and the runtime choice a GPU runtime install supersedes.
    void installM2(AppContext& context);
    // The backup coordinator and the updater, before the application controller that starts them.
    void provideM3(AppContext& context);
    // The update presenter and hand-off, the restart offer, and the reservation's window.
    void installM3(AppContext& context);
    bool applyTheme(const QString& preference);

    ControllerPlatform platform_;
    ApplicationController* app_ = nullptr;
    HistoryManager* history_ = nullptr;
    Recorder* recorder_ = nullptr;
    dictation::StreamingRuntime* streaming_ = nullptr;
    models::ModelRuntime* models_ = nullptr;
    models::ComponentRuntime* components_ = nullptr;
    std::shared_ptr<std::atomic<bool>> runtimeLoaded_ = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<ClipboardService> clipboard_;
    std::unique_ptr<EngineSetup> engineSetup_;
    // The engine setup's cancel (its adoption), cancelled by Quit's CancelRoot step.
    std::shared_ptr<CancelSource> setupCancel_ = std::make_shared<CancelSource>();
    std::unique_ptr<UiController> ui_;
    std::shared_ptr<ActionWiring> actionWiring_;
    std::unique_ptr<SettingsBindings> bindings_;
    // Lists the components and downloaded models off the GUI thread, once the window shows.
    std::function<void()> warmModelViews_;
    // The palette a Settings theme change switches when no applyTheme was given.
    std::unique_ptr<ui::ThemeManager> themeManager_;
    // Both are provided to the context as well, which keeps them alive after this feature.
    std::shared_ptr<BackupLifecycle> backup_;
    std::shared_ptr<UpdateServices> updateServices_;
    std::unique_ptr<UpdatePresenter> updates_;
    std::unique_ptr<CodeChangeRestart> codeChange_;
    std::unique_ptr<PersonalizeWiring> personalize_;
    std::vector<Subscription> links_;
};

} // namespace fv
