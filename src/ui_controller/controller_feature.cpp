#include "ui_controller/controller_feature.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <string>
#include <utility>

#include <QApplication>
#include <QGuiApplication>
#include <QMessageBox>
#include <QPointer>

#include "appcore/activity.h"
#include "appcore/cancel_router.h"
#include "appcore/shutdown_plan.h"
#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "audio/backend.h"
#include "audio/devices.h"
#include "automation/event_bus.h"
#include "backup/coordinator.h"
#include "base/cancel.h"
#include "base/env.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/paths.h"
#include "clipboard/backend.h"
#include "clipboard/render.h"
#include "clipboard/sequence.h"
#include "clipboard/temporary_clipboard.h"
#include "db/database.h"
#include "dictation/hotkey_runtime.h"
#include "dictation/recorder_port.h"
#include "dictation/streaming_runtime.h"
#include "dictation/transcription_runtime.h"
#include "engine/version.h"
#include "focus/capture_service.h"
#include "focus/synthetic_keys.h"
#include "history/dictation_stats.h"
#include "history/manager.h"
#include "hotkeys/hotkey_manager.h"
#include "hotkeys/keyboard_hook.h"
#include "hotkeys/mouse_hook.h"
#include "models/catalog.h"
#include "models/component_details.h"
#include "models/component_leases.h"
#include "models/component_runtime.h"
#include "models/component_store.h"
#include "models/downloads_model.h"
#include "models/hf_access.h"
#include "models/model_runtime.h"
#include "models/speech_catalog.h"
#include "recorder/recorder.h"
#include "remote/record_sync_control.h"
#include "settings/install.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "speech/dictation_language.h"
#include "speech/service.h"
#include "sql/db.h"
#include "ui_controller/action_wiring.h"
#include "ui_controller/application_controller.h"
#include "ui_controller/backup_lifecycle.h"
#include "ui_controller/clipboard_service.h"
#include "ui_controller/code_change_restart.h"
#include "ui_controller/engine_control.h"
#include "ui_controller/engine_setup.h"
#include "ui_controller/personalize_wiring.h"
#include "ui_controller/process_judge.h"
#include "ui_controller/settings_bindings.h"
#include "ui_controller/ui_controller.h"
#include "ui_controller/update_presenter.h"
#include "ui_downloads/downloads_presenter.h"
#include "ui_downloads/model_assignments.h"
#include "ui_history/history_feature.h"
#include "ui_history/history_store.h"
#include "ui_overlay/waveform_overlay.h"
#include "ui_record/compact_record_controller.h"
#include "ui_record/record_feature.h"
#include "ui_record/scratchpad.h"
#include "ui_record/upload_presenter.h"
#include "ui_settings/app_pages.h"
#include "ui_settings/home_page.h"
#include "ui_settings/settings_window.h"
#include "ui_setup/guided_setup_offer.h"
#include "ui_setup/runtime_setup.h"
#include "ui_shell/application.h"
#include "ui_shell/bootstrap.h"
#include "ui_shell/main_window.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"
#include "update/code_identity.h"
#include "update/download.h"
#include "update/gates.h"
#include "update/install_probe.h"
#include "update/release_feed.h"
#include "update/runtime.h"
#include "update_native/desktop.h"
#include "walkthrough/storage.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"services.application_controller"};

// What an engine resolution leaves for the speech service.
speech::BackendConfig toBackendConfig(const std::string& backend,
                                      const models::ResolvedEngine& engine) {
    speech::BackendConfig config;
    config.backend = backend;
    if (engine.runtime_dir) {
        config.runtime_dir = *engine.runtime_dir;
    }
    if (engine.model_path) {
        config.model_path = *engine.model_path;
    }
    config.model_key = engine.model_key;
    config.model_label = engine.model_label;
    if (!engine.requested_device.empty()) {
        config.requested_device = engine.requested_device;
    }
    if (!engine.device.empty()) {
        config.device = engine.device;
    }
    config.gpu_index = engine.gpu_index < 0 ? 0 : engine.gpu_index;
    config.missing_message = engine.missing_message;
    switch (engine.missing_state) {
    case models::EngineMissing::RuntimeMissing:
        config.missing_state = speech::ReadyState::RuntimeMissing;
        break;
    case models::EngineMissing::ModelMissing:
        config.missing_state = speech::ReadyState::ModelMissing;
        break;
    case models::EngineMissing::None:
        break;
    }
    return config;
}

// The real platform, for each function a test left empty.
ControllerPlatform withDefaults(ControllerPlatform platform) {
    if (!platform.keyboardHook) {
        platform.keyboardHook = [] {
            return hotkeys::make_keyboard_hook();
        };
    }
    if (!platform.mouseHook) {
        platform.mouseHook = [] {
            return hotkeys::make_mouse_hook_source();
        };
    }
    if (!platform.applyFontScale) {
        platform.applyFontScale = [](int percent) {
            if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
                ui::applyUiFontScale(percent, *app);
            }
        };
    }
    if (!platform.captureService) {
        platform.captureService = [](const SettingsStore& settings) {
            return focus::make_capture_service(settings);
        };
    }
    if (!platform.syntheticKeys) {
        platform.syntheticKeys = []() -> std::shared_ptr<focus::SyntheticKeys> {
            return focus::make_synthetic_keys();
        };
    }
    if (!platform.clipboardBackend) {
        platform.clipboardBackend = [] {
            return clipboard::qt_clipboard_backend(QGuiApplication::clipboard());
        };
    }
    if (!platform.recorder) {
        platform.recorder = [](SettingsStore& settings, audio::Backend& backend, const Paths& paths,
                               Executor& io) {
            return Recorder::from_settings(settings, backend, paths, io);
        };
    }
    if (!platform.resolveEngine) {
        platform.resolveEngine = [](const Json& settings, const Paths& paths) {
            return models::resolve_parakeet(settings, paths);
        };
    }
    if (!platform.adoptMissing) {
        platform.adoptMissing = [](const Json& settings, const Paths& paths,
                                   const CancelToken& cancel) {
            models::OpenWhisperAdopter adopter(paths);
            return adopter.adopt_missing(settings, cancel);
        };
    }
    if (!platform.recoverComponents) {
        platform.recoverComponents = [](const Paths& paths, http::Client* client) {
            if (client == nullptr) {
                return;
            }
            models::ComponentStore store(paths.components_root(), *client);
            store.prune_orphans();
            store.settle_staged_changes();
        };
    }
    if (!platform.lastCrash) {
        platform.lastCrash = [](const Paths& paths) {
            return log::CrashHandler::last_crash(paths, identity());
        };
    }
    if (!platform.offerCpuRuntime) {
        platform.offerCpuRuntime = [](QWidget* parent) {
            const QString product = qt::toQString(identity().product);
            QMessageBox box(QMessageBox::Warning, product,
                            QStringLiteral("The speech engine crashed"),
                            QMessageBox::Yes | QMessageBox::No, parent);
            box.setInformativeText(
                QStringLiteral("Use the CPU runtime instead? The change applies the next time "
                               "%1 starts.")
                    .arg(product));
            box.setDefaultButton(QMessageBox::Yes);
            return box.exec() == QMessageBox::Yes;
        };
    }
    if (!platform.acquireComponent) {
        platform.acquireComponent = [](std::string_view id) {
            // Never released: a loaded runtime stays loaded until the process ends.
            static std::mutex mutex;
            static std::vector<std::function<void()>> held;
            const std::lock_guard lock(mutex);
            held.push_back(models::acquire_component(id));
        };
    }
    if (!platform.installProbe) {
        platform.installProbe = [] {
            return update::probe_install(update::release_identity());
        };
    }
    if (!platform.updateFeed) {
        platform.updateFeed = [] {
            return update::feed_config(system_env());
        };
    }
    if (!platform.exitApp) {
        platform.exitApp = [](int code) {
            QCoreApplication::exit(code);
        };
    }
    if (!platform.warning) {
        platform.warning = [](QWidget* parent, const QString& title, const QString& text) {
            QMessageBox::warning(parent, title, text);
        };
    }
    return platform;
}

template <class T> T* findService(AppContext& context, T& (AppContext::*accessor)()) {
    try {
        return &(context.*accessor)();
    } catch (const ServiceUnavailable&) {
        return nullptr;
    }
}

// How long Quit waits for a download or install still running once it was cancelled.
constexpr std::chrono::milliseconds kModelStrandShutdownBudget{5000};

// What the model and component runtimes share (the controller's): the downloads and components
// strands (OpenWhisper's model download executor and its one-worker component_executor, kept
// off the jobs strand so a long install never starves a transcription), the speech model cache
// and its inventory, the component store and coordinator, the Hugging Face access policy, and
// the cancel source Quit fires.
struct ModelServices {
    ModelServices(const Paths& paths, http::Client& client, SettingsStore& settings,
                  ModelServiceOptions options_)
        : options(std::move(options_)), env(options.env != nullptr ? options.env : &system_env()),
          cache(paths.speech_models_root(), client, cacheOptions(options, env)), snapshot(cache),
          store(paths.components_root(), client, options.store),
          coordinator(store, coordinatorOptions(options)),
          access(
              settings,
              [this](std::string_view name) {
                  return models::model(name) != nullptr && cache.is_cached(name);
              },
              *env) {}
    ~ModelServices() { stop(); }
    ModelServices(const ModelServices&) = delete;
    ModelServices& operator=(const ModelServices&) = delete;

    static models::SpeechModelCacheOptions cacheOptions(const ModelServiceOptions& options,
                                                        const EnvReader* env) {
        models::SpeechModelCacheOptions cache = options.cache;
        if (cache.env == nullptr) {
            cache.env = env;
        }
        return cache;
    }
    static models::ComponentCoordinator::Options
    coordinatorOptions(const ModelServiceOptions& options) {
        models::ComponentCoordinator::Options coordinator = options.coordinator;
        if (!coordinator.copy_for) {
            coordinator.copy_for = &models::component_copy;
        }
        return coordinator;
    }

    // Quit: the queued downloads and installs are dropped and the running ones waited for.
    void stop() {
        downloads.shutdown(kModelStrandShutdownBudget);
        components.shutdown(kModelStrandShutdownBudget);
    }

    ModelServiceOptions options;
    const EnvReader* env;
    CancelSource shutdown;
    ThreadPool pool{"fv-models", 2};
    Strand downloads{pool, "downloads", 1, ShutdownMode::CancelQueued};
    Strand components{pool, "components", 1, ShutdownMode::CancelQueued};
    models::SpeechModelCache cache;
    models::CachedModelsSnapshot snapshot;
    models::ComponentStore store;
    models::ComponentCoordinator coordinator;
    models::HuggingFaceAccessCoordinator access;
};

// How long Quit, and the updater's release, wait for a setup download that was cancelled.
constexpr std::chrono::milliseconds kUpdateStrandShutdownBudget{5000};

} // namespace

// What the updater's checks and downloads reach (CheckContext holds references to the feed and the
// probe), its own download strand, and the cancel source Quit fires. The jobs strand, where checks
// run, is Bootstrap's: Quit shuts it down (ShutdownStep::Strands) before the context releases this.
struct ControllerFeature::UpdateServices {
    UpdateServices(update::FeedConfig feedConfig, update::InstallProbe installProbe)
        : feed(std::move(feedConfig)), probe(std::move(installProbe)) {}
    ~UpdateServices() {
        runtime.reset();
        components.shutdown(kUpdateStrandShutdownBudget);
    }
    UpdateServices(const UpdateServices&) = delete;
    UpdateServices& operator=(const UpdateServices&) = delete;

    update::FeedConfig feed;
    update::InstallProbe probe;
    CancelSource shutdown;
    // The setup download and the leftover prune, never on a strand a transcription waits on.
    ThreadPool pool{"fv-update", 1};
    Strand components{pool, "update", 1, ShutdownMode::CancelQueued};
    std::unique_ptr<update::UpdaterRuntime> runtime;
};

update::UpdaterRuntime* ControllerFeature::updater() const {
    return updateServices_ != nullptr ? updateServices_->runtime.get() : nullptr;
}

ControllerFeature::ControllerFeature() : ControllerFeature(ControllerPlatform{}) {}

ControllerFeature::ControllerFeature(ControllerPlatform platform)
    : platform_(withDefaults(std::move(platform))) {}

ControllerFeature::~ControllerFeature() {
    links_.clear();
    if (actionWiring_)
        actionWiring_->close();
    actionWiring_.reset();
    personalize_.reset();
    codeChange_.reset();
    updates_.reset();
    bindings_.reset();
    ui_.reset();
    engineSetup_.reset();
    clipboard_.reset();
}

// application_controller.py:165-275, in its construction order.
void ControllerFeature::provide(AppContext& context) {
    SettingsStore& settings = context.settings();
    // First, so nothing this launch writes the settings file before a brand-new install is
    // told apart.
    seed_new_install_settings(settings);
    // The process's one judge provider, before anything that judges (actions, the Actions page,
    // Fast judgments' key check) looks for it.
    provideProcessJudge(context);

    const Paths& paths = context.paths();
    Executor& ui = context.ui();
    Executor& io = context.io();
    audio::Backend& audioBackend = context.audio();
    speech::SpeechService& speech = context.speech();
    Database& database = context.db();
    ActivityState& activity = context.activity();
    // The app's events go to the process-wide bus the live API, hooks and MCP Events follow,
    // unless the context names another sink (Bootstrap names none).
    EventSink* events = findService(context, &AppContext::events);
    if (events == nullptr) {
        events = &automation::bus();
    }

    // The recorder from the saved microphone order.
    std::shared_ptr<Recorder> recorder = platform_.recorder(settings, audioBackend, paths, io);
    recorder_ = recorder.get();
    context.provide<Recorder>(recorder);
    auto recorderPort = std::make_shared<dictation::RecorderAdapter>(*recorder);
    context.provide<dictation::RecorderAdapter>(recorderPort);

    std::shared_ptr<focus::CaptureService> capture = platform_.captureService(settings);
    focus::set_service(capture);
    context.provide<focus::CaptureService>(capture);
    std::shared_ptr<focus::SyntheticKeys> keys = platform_.syntheticKeys();
    context.provide<focus::SyntheticKeys>(keys);
    auto temporaryClipboard = std::make_shared<clipboard::TemporaryClipboard>(
        platform_.clipboardBackend(), clipboard::system_clipboard_sequence(),
        clipboard::system_renderer());
    context.provide<clipboard::TemporaryClipboard>(temporaryClipboard);
    clipboard_ = std::make_unique<ClipboardService>(temporaryClipboard, keys);

    // One StatsRepository for the database: its listeners belong to the instance, and its
    // writes are serialized per instance.
    auto stats = std::make_shared<dictation_stats::StatsRepository>(database, context.clock());
    context.provide<dictation_stats::StatsRepository>(stats);
    HistoryServices historyServices;
    historyServices.clock = &context.clock();
    // The record sync RemoteFeature provides when it builds the remote engine; every provider
    // is registered while the app is composed, before anything is saved to History.
    historyServices.find_record_sync = [&context]() -> RecordSync* {
        return context.find<remote::RecordSyncControl>();
    };
    // Deleted history must not be replayed to an event stream that reconnects.
    historyServices.forget_replayable_events = [] {
        automation::clear_bus();
    };
    auto history =
        std::make_shared<HistoryManager>(database, HistoryManager::default_recordings_folder(paths),
                                         *settings.snapshot(), std::move(historyServices));
    history_ = history.get();
    context.provide<HistoryManager>(history);

    dictation::TranscriptionServices services;
    services.settings = &settings;
    services.recorder = recorderPort.get();
    services.speech = &speech;
    services.history = history.get();
    services.stats = stats.get();
    services.delivery.clipboard = temporaryClipboard.get();
    services.delivery.keys = keys.get();
    services.activity = &activity;
    services.ui = &ui;
    services.jobs = &context.jobs();
    services.persistence = &context.persistence();
    services.events = events;
    services.capture = capture.get();
    services.clock = &context.clock();
    // Startup recovery joins interrupted walkthroughs to their recovered audio and prunes
    // expired ones there.
    services.captures_root = walkthrough::captures_root(paths);
    // The runtime is built before the application controller it calls back, and both live in
    // the context, which outlives this feature: the hooks reach the controller through a weak
    // reference, never through the feature.
    auto appSlot = std::make_shared<std::weak_ptr<ApplicationController>>();
    services.hooks.readiness_message = [appSlot]() -> std::optional<std::string> {
        const std::shared_ptr<ApplicationController> app = appSlot->lock();
        return app != nullptr ? app->transcription_readiness_message() : std::nullopt;
    };
    services.hooks.request_reload = [appSlot, &ui] {
        ui.post([appSlot] {
            if (const auto app = appSlot->lock()) {
                app->reload_whisper_model();
            }
        });
    };
    services.hooks.request_model_consent = [appSlot, &ui] {
        ui.post([appSlot] {
            if (const auto app = appSlot->lock()) {
                app->model_setup_requested.emit();
            }
        });
    };
    services.hooks.minimize_to_tray = [appSlot] {
        if (const auto app = appSlot->lock()) {
            app->minimize_to_tray();
        }
    };
    services.hooks.paste_last_original = [appSlot] {
        if (const auto app = appSlot->lock()) {
            app->paste_last_original();
        }
    };
    auto transcription = std::make_shared<dictation::TranscriptionRuntime>(std::move(services));
    context.provide<dictation::TranscriptionRuntime>(transcription);

    provideM3(context);

    // The backend table (_setup_transcription_backends): the saved engine resolves to its
    // runtime folder and model at each load.
    auto runtimeId = std::make_shared<std::string>();
    ApplicationServices appServices;
    appServices.settings = &settings;
    appServices.activity = &activity;
    appServices.speech = &speech;
    appServices.transcription = transcription.get();
    appServices.recorder = recorder.get();
    appServices.cancel = &context.cancel();
    appServices.ui = &ui;
    appServices.startup = &io;
    appServices.jobs = &context.jobs();
    appServices.io = &io;
    const Paths* pathsPtr = &paths;
    appServices.resolve_engine = [resolve = platform_.resolveEngine, pathsPtr,
                                  runtimeId](const Json& snapshot) {
        const std::string backend = speech::engine(snapshot);
        if (backend != "parakeet") {
            speech::BackendConfig config;
            config.backend = backend;
            return config;
        }
        const models::ResolvedEngine engine = resolve(snapshot, *pathsPtr);
        *runtimeId = engine.runtime_id;
        return toBackendConfig(backend, engine);
    };
    appServices.ensure_database = [&database] {
        // The database Bootstrap opened answers; a broken file shows its error here.
        database.read([](sql::Connection& connection) { connection.scalar("SELECT 1"); });
    };
    appServices.load_priority = [&settings, &audioBackend] {
        return audio::load_priority(settings, audioBackend);
    };
    appServices.runtime_loaded = [acquire = platform_.acquireComponent, loaded = runtimeLoaded_,
                                  runtimeId](const speech::BackendConfig&) {
        if (!loaded->exchange(true) && !runtimeId->empty()) {
            acquire(*runtimeId);
        }
    };
    // What the controller asks of the runtimes built after it: the live preview's set-up and the
    // model runtime's forgotten questions. Each slot is cleared when its runtime goes.
    auto streamingSlot = std::make_shared<std::atomic<dictation::StreamingRuntime*>>(nullptr);
    auto modelsSlot = std::make_shared<std::atomic<models::ModelRuntime*>>(nullptr);
    appServices.setup_streaming = [streamingSlot] {
        if (dictation::StreamingRuntime* streaming = *streamingSlot) {
            streaming->setup();
        }
    };
    appServices.forget_runtime_prompts = [modelsSlot] {
        if (models::ModelRuntime* runtime = *modelsSlot) {
            runtime->forget_runtime_prompts();
        }
    };
    // notify_main_ui_ready's updater jobs: the leftovers pruned, then the launch check once
    // startup is out of the way. Both services were provided before the controller, so they are
    // released after it.
    std::weak_ptr<UpdateServices> weakUpdates = updateServices_;
    appServices.start_background_jobs = [weakUpdates] {
        const std::shared_ptr<UpdateServices> updates = weakUpdates.lock();
        if (updates == nullptr || updates->runtime == nullptr) {
            return;
        }
        updates->runtime->prune_leftovers();
        updates->runtime->schedule_check(true);
    };
    std::weak_ptr<BackupLifecycle> weakBackup = backup_;
    appServices.start_database_dependents = [weakBackup] {
        if (const std::shared_ptr<BackupLifecycle> backup = weakBackup.lock()) {
            backup->start();
        }
    };
    auto app = std::make_shared<ApplicationController>(std::move(appServices));
    app_ = app.get();
    *appSlot = app;
    context.provide<ApplicationController>(app);

    // The hotkeys: the manager installs the keyboard hook; the runtime applies the saved
    // shortcuts once the UI can show them (install()).
    auto manager = std::make_shared<hotkeys::HotkeyManager>(std::map<std::string, std::string>{},
                                                            platform_.keyboardHook());
    context.provide<hotkeys::HotkeyManager>(manager);
    // The side buttons: the manager was built over the controller's keyboard hook, so it hooks
    // no mouse until it is given one.
    if (std::unique_ptr<hotkeys::MouseHookSource> mouse = platform_.mouseHook()) {
        manager->set_mouse_hook_source(std::move(mouse));
    }
    auto hotkeyRuntime = std::make_shared<dictation::HotkeyRuntime>(*manager, *app, settings, ui,
                                                                    context.clock(), *keys);
    app->set_hotkey_runtime(hotkeyRuntime.get());
    context.provide<dictation::HotkeyRuntime>(hotkeyRuntime);

    // AI cleanup, dictionary learning, Command Mode and transforms, and rule dictation: provided
    // after the runtimes they attach to, so they are released (and detached) first.
    PersonalizeParts personalizeParts;
    personalizeParts.transcription = transcription.get();
    personalizeParts.hotkeys = hotkeyRuntime.get();
    personalizeParts.app = app.get();
    personalizeParts.clipboard = temporaryClipboard.get();
    personalizeParts.keys = keys.get();
    personalizeParts.capture = capture.get();
    personalizeParts.keyEnvironment = platform_.keyEnvironment;
    personalize_ = std::make_unique<PersonalizeWiring>();
    personalize_->provide(context, personalizeParts);

    // The live preview (streaming.py), over the dictation recorder. It follows the engine's
    // reloads itself (attach: a set-up waiting for the engine runs on streaming_setup_due); the
    // transcription runtime, provided before it and so released after it, lets go of it first.
    dictation::TranscriptionRuntime* transcriptionPtr = transcription.get();
    std::shared_ptr<dictation::StreamingRuntime> streaming(
        new dictation::StreamingRuntime(speech, *recorderPort, settings, ui, context.clock()),
        [transcriptionPtr, streamingSlot](dictation::StreamingRuntime* runtime) {
            *streamingSlot = nullptr;
            transcriptionPtr->set_streaming(nullptr);
            delete runtime;
        });
    streaming->attach(app->reload_machine());
    transcription->set_streaming(streaming.get());
    streaming_ = streaming.get();
    *streamingSlot = streaming.get();
    context.provide<dictation::StreamingRuntime>(streaming);

    // The model and component runtimes (models.py, components.py) need the network client; a
    // context without one (a test of the M1 graph) has neither, and the runtime setup answers a
    // missing model instead.
    if (http::Client* client = findService(context, &AppContext::http)) {
        ModelServiceOptions options;
        if (platform_.modelServices) {
            platform_.modelServices(options);
        }
        auto shared = std::make_shared<ModelServices>(paths, *client, settings, std::move(options));
        // The Downloads page and Home read the same inventory and coordinator (aliases of one
        // object: released together, after the runtimes provided below).
        context.provide<models::SpeechModelCache>(
            std::shared_ptr<models::SpeechModelCache>(shared, &shared->cache));
        context.provide<models::CachedModelsSnapshot>(
            std::shared_ptr<models::CachedModelsSnapshot>(shared, &shared->snapshot));
        context.provide<models::ComponentCoordinator>(
            std::shared_ptr<models::ComponentCoordinator>(shared, &shared->coordinator));
        context.provide<models::HuggingFaceAccessCoordinator>(
            std::shared_ptr<models::HuggingFaceAccessCoordinator>(shared, &shared->access));

        EngineControlPorts enginePorts;
        enginePorts.speech = &speech;
        enginePorts.app = app.get();
        enginePorts.transcription = transcription.get();
        enginePorts.activity = &activity;
        enginePorts.settings = &settings;
        enginePorts.components_root = paths.components_root();
        auto engine = std::make_shared<ControllerEngineControl>(std::move(enginePorts));
        context.provide<models::EngineControl>(engine);

        auto componentRuntime = std::make_shared<models::ComponentRuntime>(
            *engine, shared->coordinator, ui, shared->components, context.clock());
        components_ = componentRuntime.get();
        context.provide<models::ComponentRuntime>(componentRuntime);
        // Quit asks first while a component installs: OpenWhisper's get_component_installing
        // reads component_coordinator.is_any_installing() (application_controller.py:380).
        models::ComponentCoordinator* installs = &shared->coordinator;
        const auto installing = [&activity, installs] {
            activity.set_component_installing(installs->is_any_installing());
        };
        links_.push_back(componentRuntime->component_install_started.connect(
            ui, [installing](const std::string&) { installing(); }));
        links_.push_back(componentRuntime->component_install_finished.connect(
            ui, [installing](const std::string&, bool, const std::string&) { installing(); }));

        models::ModelRuntimeOptions runtimeOptions;
        runtimeOptions.shutdown = shared->shutdown.token();
        runtimeOptions.env = shared->env;
        runtimeOptions.snapshot = &shared->snapshot;
        std::shared_ptr<models::ModelRuntime> modelRuntime(
            new models::ModelRuntime(*engine, shared->cache, shared->access, shared->coordinator,
                                     settings, ui, shared->downloads, context.clock(),
                                     runtimeOptions),
            [modelsSlot](models::ModelRuntime* runtime) {
                *modelsSlot = nullptr;
                delete runtime;
            });
        // Accepted runtime questions install through the component runtime (released after it).
        modelRuntime->set_component_runtime(componentRuntime.get());
        models_ = modelRuntime.get();
        *modelsSlot = modelRuntime.get();
        context.provide<models::ModelRuntime>(modelRuntime);

        // A job that finds its model missing starts the download consent flow; the hook runs on
        // the job's thread and the runtime is a UI actor.
        transcription->set_model_consent_hook([modelsSlot, &ui] {
            ui.post([modelsSlot] {
                if (models::ModelRuntime* runtime = *modelsSlot) {
                    runtime->ensure_selected_model_available();
                }
            });
        });

        // notify_main_ui_ready's warm-ups: the installed components are measured (a walk of
        // their files) and the downloaded models listed, so that Home and Downloads never do it
        // on the GUI thread.
        std::weak_ptr<ModelServices> weakServices = shared;
        warmModelViews_ = [weakServices, &io] {
            io.post([weakServices] {
                const std::shared_ptr<ModelServices> held = weakServices.lock();
                if (held == nullptr) {
                    return;
                }
                try {
                    held->coordinator.list_components();
                    held->snapshot.scan(30.0);
                } catch (const std::exception& error) {
                    kLog.warning("Couldn't list the installed models and components: {}",
                                 error.what());
                }
            });
        };

        if (ShutdownPlan* plan = findService(context, &AppContext::shutdown)) {
            ModelServices* owned = shared.get();
            plan->add(ShutdownStep::CancelRoot, {}, [owned] { owned->shutdown.cancel(); });
            plan->add(ShutdownStep::ModelBatchDownload, "Error cancelling model batch download",
                      [modelsSlot] {
                          if (models::ModelRuntime* runtime = *modelsSlot) {
                              runtime->stop_batch();
                          }
                      });
            plan->add(ShutdownStep::ComponentInstalls, "Error cancelling component installs",
                      [owned] { owned->coordinator.cancel_all(); });
            plan->add(ShutdownStep::Strands, "Error during executor shutdown",
                      [owned] { owned->stop(); });
        }
    }

    // The microphone moved to another one mid-recording.
    links_.push_back(
        recorder->device_switched.connect(ui, [this](std::string oldName, std::string newName) {
            if (app_ != nullptr) {
                app_->recording_device_switched.emit(oldName, newName);
            }
        }));

    // Quit (A03-29): the controller's steps; the strands, the persistence drain and the
    // database close are added by whoever owns them.
    if (ShutdownPlan* plan = findService(context, &AppContext::shutdown)) {
        // cleanup's first step: the backup coordinator, after the app is marked as shutting
        // down, so its reservation's end leaves the window alone.
        BackupLifecycle* backupPtr = backup_.get();
        plan->add(ShutdownStep::BackupCoordinator, {}, [backupPtr] { backupPtr->shutdown(); });
        app->add_shutdown_steps(*plan);
        UpdateServices* updates = updateServices_.get();
        plan->add(ShutdownStep::CancelRoot, {}, [updates] { updates->shutdown.cancel(); });
        // _stop_timers ends with updater.stop().
        plan->add(ShutdownStep::Timers, "Error stopping watchdog timers", [updates] {
            if (updates->runtime != nullptr) {
                updates->runtime->stop();
            }
        });
        plan->add(ShutdownStep::Strands, "Error during executor shutdown",
                  [updates] { updates->components.shutdown(kUpdateStrandShutdownBudget); });
        hotkeys::HotkeyManager* managerPtr = manager.get();
        plan->add(ShutdownStep::HotkeyManager, "Error during hotkey cleanup",
                  [managerPtr] { managerPtr->cleanup(); });
        // streaming_runtime.cleanup(), before the strands are waited for: a stop still
        // finalizing its preview on the jobs strand returns at once instead of decoding.
        plan->add(ShutdownStep::Streaming, "Error during streaming cleanup", [streamingSlot] {
            if (dictation::StreamingRuntime* streaming = *streamingSlot) {
                streaming->cleanup();
            }
        });
        plan->add(ShutdownStep::UiController, "Error during UI controller cleanup", [this] {
            if (codeChange_ != nullptr) {
                codeChange_->stop();
            }
            if (ui_ != nullptr) {
                ui_->cleanup();
            }
        });
    }

    auto lifecycle = std::make_shared<ShellLifecycle>();
    lifecycle->trayCreated = [this](SystemTray& tray) {
        if (ui_ != nullptr) {
            ui_->attachTray(tray);
        }
        if (updates_ != nullptr) {
            updates_->attachTray(tray);
        }
    };
    const std::filesystem::path updatesDir = update::updates_root(paths);
    lifecycle->mainWindowShown = [this, updatesDir, &context] {
        if (engineSetup_ == nullptr || app_ == nullptr) {
            return;
        }
        // bootstrap.py shows a leftover update error before anything else asks the user a
        // question. The health launch that skips it is M10's.
        if (!context.find<UpdateHealthLaunch>()) {
            showApplyErrorIfAny(updatesDir, context.find<MainWindow>(), platform_.warning);
        }
        ApplicationController* app = app_;
        engineSetup_->start([app] { app->notify_main_ui_ready(); });
        if (warmModelViews_) {
            warmModelViews_();
        }
    };
    lifecycle->ambientStatus = [this](const std::string& status) {
        if (ui_ != nullptr) {
            ui_->setAmbientStatus(qt::toQString(status));
        }
    };
    context.provide<ShellLifecycle>(lifecycle);
}

void ControllerFeature::install(AppContext& context) {
    MainWindow& window = context.service<MainWindow>();
    RecordWidgets& record = context.service<RecordWidgets>();
    // The compact controller replaces the tabs in Compact Mode.
    window.setCompactController(record.compactController());

    SettingsStore& settings = context.settings();
    const Paths* paths = &context.paths();
    http::Client* client = findService(context, &AppContext::http);
    RuntimeSetup* setup = context.find<RuntimeSetup>();
    if (setup != nullptr) {
        // Whether a runtime is loaded decides whether a new runtime folder needs a restart.
        setup->setRuntimeLoaded([loaded = runtimeLoaded_] { return loaded->load(); });
    }

    EngineSetupPorts ports;
    ports.settings = &settings;
    ports.io = &context.io();
    ports.ui = &context.ui();
    ports.recover_components = [this, paths, client] {
        platform_.recoverComponents(*paths, client);
    };
    ports.adopt_missing = [this, paths](const Json& snapshot, const CancelToken& cancel) {
        return platform_.adoptMissing(snapshot, *paths, cancel);
    };
    ports.cancel = setupCancel_->token();
    if (ShutdownPlan* plan = findService(context, &AppContext::shutdown)) {
        plan->add(ShutdownStep::CancelRoot, {}, [cancel = setupCancel_] { cancel->cancel(); });
    }
    ports.resolve = [this, paths](const Json& snapshot) {
        return platform_.resolveEngine(snapshot, *paths);
    };
    QPointer<MainWindow> windowPtr = &window;
    ports.ensure_engine = [this, setup, windowPtr]() {
        if (platform_.ensureEngine) {
            return platform_.ensureEngine(windowPtr.data());
        }
        return setup != nullptr && setup->ensureEngine(windowPtr.data());
    };
    ports.last_crash = [this, paths] {
        return platform_.lastCrash(*paths);
    };
    ports.offer_cpu_runtime = [this, windowPtr] {
        return platform_.offerCpuRuntime(windowPtr.data());
    };
    ports.crash_offered_file = log::CrashHandler::crash_log_file(*paths, identity());
    ports.crash_offered_file += ".offered";
    engineSetup_ = std::make_unique<EngineSetup>(std::move(ports));

    UiControllerParts parts;
    parts.window = &window;
    parts.screens = &context.screens();
    parts.record = &record;
    parts.overlay = &context.service<WaveformOverlay>();
    parts.app = app_;
    parts.transcription = &context.service<dictation::TranscriptionRuntime>();
    parts.hotkeys = &context.service<dictation::HotkeyRuntime>();
    parts.clipboard = clipboard_.get();
    parts.activity = &context.activity();
    parts.recorder = recorder_;
    // The History panel installs only over a HistoryManager, which provide() made.
    parts.history = context.find<HistoryPanel>();
    if (history_ != nullptr) {
        parts.historyStore = make_history_store(*history_);
    }
    parts.engineSetup = engineSetup_.get();
    parts.ui = &context.ui();
    parts.io = &context.io();
    parts.settings = &settings;
    parts.upload = context.find<UploadPresenter>();
    parts.streaming = streaming_;
    parts.scratchpad = context.find<ScratchpadService>();
    parts.models = models_;
    parts.components = components_;
    parts.guidedSetup = context.find<GuidedSetup>();
    parts.downloads = context.find<DownloadsPresenter>();
    parts.settingsWindow = context.find<SettingsWindow>();
    ui_ = std::make_unique<UiController>(std::move(parts));

    try {
        actionWiring_ = ActionWiring::make(context, *app_, *ui_, *clipboard_);
        context.provide<ActionWiring>(actionWiring_);
    } catch (const ServiceUnavailable&) {
        // Headless and reduced test contexts can lack a network client or credential store.
    }

    installM2(context);
    installM3(context);
    personalize_->install(context, *ui_);

    // Python's constructor ends here: the shortcuts are applied and announced to the UI just
    // wired, then the watchdog starts.
    auto& hotkeys = context.service<dictation::HotkeyRuntime>();
    hotkeys.connect_signals();
    hotkeys.setup_hotkeys();
    hotkeys.setup_hook_watchdog();
}

// The M2 half of install(): what the controller connects between the features it does not own.
void ControllerFeature::installM2(AppContext& context) {
    SettingsStore& settings = context.settings();
    Executor& ui = context.ui();

    SettingsBindingsParts parts;
    parts.signals = context.find<SettingsPagesSignals>();
    parts.window = context.find<SettingsWindow>();
    parts.app = app_;
    parts.ui = ui_.get();
    parts.hotkeys = &context.service<dictation::HotkeyRuntime>();
    parts.streaming = streaming_;
    parts.record = &context.service<RecordWidgets>();
    parts.overlay = &context.service<WaveformOverlay>();
    parts.screens = &context.screens();
    parts.downloads = context.find<DownloadsPresenter>();
    // _apply_ui_theme: through the application's own theme manager when Bootstrap provided it.
    Application* application = context.find<Application>();
    parts.applyTheme = [this, application](const QString& preference) {
        if (!platform_.applyTheme && application != nullptr) {
            return application->setTheme(preference);
        }
        return applyTheme(preference);
    };
    parts.applyFontScale = platform_.applyFontScale;
    bindings_ = std::make_unique<SettingsBindings>(std::move(parts));

    // The guided setup shows the progress of the download it asked for itself; the Downloads
    // presenter's own progress dialog stays out of it.
    GuidedSetup* guided = context.find<GuidedSetup>();
    if (auto* downloads = context.find<DownloadsPresenter>(); downloads != nullptr && guided) {
        QPointer<GuidedSetup> setup = guided;
        downloads->setDownloadShownElsewhere([setup](const QString& model) {
            return setup != nullptr && setup->showsDownload(model);
        });
    }

    // What Home reads of the Voice model and Downloads pages, which it cannot include.
    if (auto* home = context.find<HomeSources>()) {
        home->voiceDetail = [](const Json& snapshot) {
            return voice_detail(snapshot);
        };
        home->voiceIsRemote = [](const Json& snapshot) {
            return voice_is_remote(snapshot);
        };
        home->voiceIsCloud = [](const Json& snapshot) {
            return voice_is_cloud(snapshot);
        };
        // models.on_backend_changed = select_transcription_backend, for an engine chosen on Home
        // while the Voice model page is not built (the built page has its own handler).
        home->backendChanged = [ui = ui_.get()](const QString& displayName) {
            ui->onModelChanged(displayName);
        };
        home->runtimeSummary = [loaded = runtimeLoaded_](const Json& snapshot) {
            return runtime_summary(snapshot, loaded->load());
        };
        if (auto* snapshot = context.find<models::CachedModelsSnapshot>()) {
            home->storage = [snapshot]() -> std::optional<models::StorageSummary> {
                const auto inventory = snapshot->peek();
                if (!inventory) {
                    return std::nullopt;
                }
                return models::storage_summary(*inventory);
            };
        }
        if (auto* coordinator = context.find<models::ComponentCoordinator>()) {
            home->components = [coordinator] {
                return coordinator->list_components();
            };
        }
    }

    // ComponentRuntime._on_install_finished's GPU branch: a GPU speech runtime that installed
    // supersedes the CPU runtime the crash offer (or the user) chose; the choice goes back to
    // "not chosen", so the next start picks the GPU runtime, as the finish message says.
    if (components_ != nullptr) {
        SettingsStore* store = &settings;
        links_.push_back(components_->component_install_finished.connect(
            ui, [store](std::string componentId, bool success, std::string) {
                const bool gpu = componentId == models::component_id::kAsrNvidiaCuda ||
                                 componentId == models::component_id::kAsrNvidiaVulkan;
                if (!success || !gpu || keys::fv_speech_runtime.read(*store->snapshot()) != "cpu") {
                    return;
                }
                kLog.info("GPU runtime installed - moving the speech runtime choice from 'cpu' "
                          "back to automatic");
                store->save_setting(keys::fv_speech_runtime.key(), Json(""));
            }));
    }
}

// _apply_ui_theme without the Application in the context (a composition other than
// Bootstrap's): a theme manager the controller keeps, which follows the platform colour scheme
// as the application's does; applyUiTheme remembers it for the font scale from then on.
bool ControllerFeature::applyTheme(const QString& preference) {
    if (platform_.applyTheme) {
        return platform_.applyTheme(preference);
    }
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (app == nullptr) {
        return false;
    }
    if (themeManager_ == nullptr) {
        themeManager_ = std::make_unique<ui::ThemeManager>(
            ui::resolveThemePreference(ui::currentUiThemePreference(), app));
        ui::followPlatformColorScheme(*app, *themeManager_);
    }
    return ui::applyUiTheme(preference, *app, *themeManager_);
}

// setup_backup (application_controller.py:741-749) and the updater of
// services/runtime/updater.py. Python built the coordinator once the window showed; it is built
// here, before any feature installs, so the Backup page and the import find it in the context.
// Neither starts anything until the startup jobs do.
void ControllerFeature::provideM3(AppContext& context) {
    BackupLifecyclePorts backupPorts;
    backupPorts.paths = &context.paths();
    backupPorts.identity = &context.identity();
    backupPorts.activity = &context.activity();
    backupPorts.ui = &context.ui();
    backupPorts.clock = &context.clock();
    backupPorts.service = platform_.backupService;
    backupPorts.centralWidget = [&context]() -> QWidget* {
        MainWindow* window = context.find<MainWindow>();
        return window != nullptr ? window->centralWidget() : nullptr;
    };
    backupPorts.exitApp = platform_.exitApp;
    backupPorts.ruleDictationBusy = [&context] {
        return ruleDictationBusy(context);
    };
    backup_ = std::make_shared<BackupLifecycle>(std::move(backupPorts));
    context.provide<BackupLifecycle>(backup_);
    context.provide<backup::BackupCoordinator>(
        std::shared_ptr<backup::BackupCoordinator>(backup_, &backup_->coordinator()));

    updateServices_ =
        std::make_shared<UpdateServices>(platform_.updateFeed(), platform_.installProbe());
    // Checks and downloads need the network client; a context without one (a test of the M1
    // graph) has no updater, and its notice never lights.
    if (http::Client* client = findService(context, &AppContext::http)) {
        Clock& clock =
            platform_.updaterClock != nullptr ? *platform_.updaterClock : context.clock();
        Executor& ui = platform_.updaterUi != nullptr ? *platform_.updaterUi : context.ui();
        update::CheckContext check{*client,
                                   updateServices_->feed,
                                   context.settings(),
                                   updateServices_->probe,
                                   clock,
                                   updateServices_->shutdown.token(),
                                   std::string(kVersion)};
        auto native = update_native::native_update_actions(context.paths(), updateServices_->probe);
        check.native_install = static_cast<bool>(native.prepare);
        check.platform = platform_.updatePlatform;
        updateServices_->runtime = std::make_unique<update::UpdaterRuntime>(
            std::move(check), update::updates_root(context.paths()), ui, context.jobs(),
            updateServices_->components, clock, std::string{}, std::move(native));
        context.provide<update::UpdaterRuntime>(std::shared_ptr<update::UpdaterRuntime>(
            updateServices_, updateServices_->runtime.get()));
    }
}

void ControllerFeature::installM3(AppContext& context) {
    MainWindow& window = context.service<MainWindow>();
    QPointer<MainWindow> windowPtr = &window;

    UpdatePresenterParts parts;
    parts.window = &window;
    parts.settings = &context.settings();
    parts.activity = &context.activity();
    parts.ui = &context.ui();
    parts.runtime = updater();
    parts.probe = &updateServices_->probe;
    parts.platform = platform_.updatePlatform;
    parts.ambientStatus = [this](const QString& status) {
        if (ui_ != nullptr) {
            ui_->setAmbientStatus(status);
        }
    };
    parts.settingsChanged = [this](std::string_view kind) {
        if (ui_ != nullptr) {
            ui_->settingsPageChanged(qt::toQString(kind));
        }
    };
    parts.openUrl = platform_.openUrl;
    parts.showDialog = platform_.showUpdateDialog;
    parts.aboutBox = platform_.aboutBox;
    const std::string_view repo = update::feed_repo(updateServices_->feed);
    if (!repo.empty()) {
        parts.releasesPage = qt::toQString(update::github_urls(repo).releases_page);
    }
    UpdateHandoffPorts& handoff = parts.handoff;
    handoff.status = [this](const QString& status) {
        if (ui_ != nullptr) {
            ui_->setStatus(status);
        }
    };
    // The app mutexes main() entered (update::LaunchGate, provided to the context by the
    // executable); a composition without one holds none.
    handoff.releaseForSetup = [&context] {
        if (auto* gate = context.find<update::LaunchGate>()) {
            gate->release_for_setup();
        }
    };
    handoff.reacquire = [&context] {
        auto* gate = context.find<update::LaunchGate>();
        return gate == nullptr || gate->reacquire();
    };
    handoff.startDetached = platform_.startDetached;
    handoff.armWatchdog = platform_.armWatchdog;
    handoff.exitApp = platform_.exitApp;
    handoff.question = platform_.question;
    handoff.warning = platform_.warning;
    updates_ = std::make_unique<UpdatePresenter>(std::move(parts));

    // _on_settings_page_changed: "updates" refreshes the notice before the application
    // controller hears of it.
    UpdatePresenter* presenter = updates_.get();
    ui_->onSettingsChanged = [presenter, previous = ui_->onSettingsChanged](std::string_view kind) {
        if (kind == "updates") {
            presenter->refreshNotice();
        }
        if (previous) {
            previous(kind);
        }
    };
    ui_->aboutBox = [presenter] {
        presenter->showAbout();
    };

    CodeChangeRestartPorts restart;
    restart.settings = &context.settings();
    restart.parent = [windowPtr]() -> QWidget* {
        return windowPtr.data();
    };
    restart.busyWork = [presenter] {
        return presenter->handoff().busyWork();
    };
    restart.changed = platform_.codeChanged;
    restart.execBox = platform_.execRestartOffer;
    ActivityState* activity = &context.activity();
    restart.confirmQuit = [this, windowPtr, activity] {
        return confirmQuitWhileBusy(windowPtr.data(), activity->snapshot(),
                                    ui_ != nullptr ? ui_->quitPrompt : QuitPrompt{});
    };
    restart.restart = [windowPtr, exitApp = platform_.exitApp] {
        restartApp(
            [windowPtr] {
                if (windowPtr != nullptr) {
                    windowPtr->saveGeometryNow();
                }
            },
            exitApp);
    };
    codeChange_ = std::make_unique<CodeChangeRestart>(std::move(restart));
    codeChange_->start();
    if (auto* lifecycle = context.find<ShellLifecycle>()) {
        lifecycle->offerRestartForFailure = [offer = QPointer<CodeChangeRestart>(codeChange_.get()),
                                             changed =
                                                 platform_.codeChanged](const QString& failure) {
            if (!offer || !(changed ? changed() : update::CodeIdentity::changed_since_launch())) {
                return false;
            }
            offer->offer(failure);
            return true;
        };
    }
}
} // namespace fv
