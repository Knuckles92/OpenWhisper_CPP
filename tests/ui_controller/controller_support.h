#pragma once

// The controller's feature graph as Bootstrap builds it, offscreen, over fakes: a temporary data
// folder and database, the fake audio system with one microphone, the speech service on a fake
// engine, a fake keyboard hook and synthetic keys, a capture service that names one app, and the
// offscreen platform's own clipboard (private to the test process). Nothing reaches the
// desktop: no hook is installed, no key is sent, no device is opened and no window is shown on
// the owner's screen.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QApplication>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QRect>
#include <QString>
#include <QStringList>

#include "appcore/activity.h"
#include "appcore/cancel_router.h"
#include "appcore/shutdown_plan.h"
#include "appkit/app_context.h"
#include "appkit/feature.h"
#include "appkit/qt_bridge.h"
#include "audio/testing/fake_backend.h"
#include "base/env.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/map_env_reader.h"
#include "credentials/store.h"
#include "db/database.h"
#include "dictation/hotkey_runtime.h"
#include "dictation/transcription_runtime.h"
#include "focus/capture_service.h"
#include "focus/testing/fake_synthetic_keys.h"
#include "hotkeys/debouncer.h"
#include "hotkeys/key_event.h"
#include "hotkeys/testing/fake_keyboard_hook.h"
#include "hotkeys/testing/fake_mouse_hook.h"
#include "http/client.h"
#include "models/catalog.h"
#include "models/engine_resolution.h"
#include "recorder/recorder.h"
#include "settings/testing/memory_store.h"
#include "speech/service.h"
#include "speech/testing/fake_backend.h"
#include "testkit/testkit.h"
#include "ui_controller/application_controller.h"
#include "ui_controller/controller_feature.h"
#include "ui_controller/ui_controller.h"
#include "ui_downloads/downloads_feature.h"
#include "ui_history/history_feature.h"
#include "ui_overlay/overlay_feature.h"
#include "ui_overlay/waveform_overlay.h"
#include "ui_personalize/personalize_feature.h"
#include "ui_record/quick_record_tab.h"
#include "ui_record/record_feature.h"
#include "ui_settings/settings_feature.h"
#include "ui_setup/setup_feature.h"
#include "ui_shell/bootstrap.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_shell/system_tray.h"
#include "ui_walkthrough/walkthrough_feature.h"
#include "update/check.h"
#include "update/release_identity.h"

namespace fvtest {

using namespace std::chrono_literals;

// Runs the event loop until done() or the time is up; returns done().
inline bool waitUntil(const std::function<bool()>& done,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(10000)) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeout.count()) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}

// Runs fn on a thread of its own (blocking database calls must not run on the GUI thread)
// while the event loop keeps turning, and returns what it returned.
template <class Fn> auto offGui(Fn fn) {
    auto future = std::async(std::launch::async, std::move(fn));
    while (future.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return future.get();
}

// A capture service that always names one app.
class NamedAppCapture final : public fv::focus::CaptureService {
public:
    NamedAppCapture(std::string appId, std::string name) {
        identity_.app_id = std::move(appId);
        identity_.name = std::move(name);
        identity_.platform = "win32";
    }
    std::shared_ptr<fv::focus::FocusRequest> request(bool, bool) override {
        fv::focus::FocusSnapshot snapshot;
        snapshot.identity = identity_;
        return fv::focus::FocusRequest::resolved(snapshot);
    }
    std::optional<fv::focus::AppIdentity> current_identity() override { return identity_; }
    void reread(const std::optional<fv::focus::AppIdentity>&,
                std::function<void(std::optional<fv::focus::TextContext>)> callback) override {
        callback(std::nullopt);
    }
    void shutdown() override {}

private:
    fv::focus::AppIdentity identity_;
};

// What the rig's platform functions were asked, in order, from any thread.
class CallLog {
public:
    void add(std::string call) {
        std::lock_guard lock(mutex_);
        calls_.push_back(std::move(call));
    }
    std::vector<std::string> all() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }
    bool has(const std::string& call) const {
        for (const auto& item : all()) {
            if (item == call) {
                return true;
            }
        }
        return false;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> calls_;
};

struct RigOptions {
    fv::Json settings =
        fv::Json{{"auto_paste", true}, {"copy_clipboard", true}, {"restore_clipboard", true}};
    fv::speech::FakeScript script = [] {
        fv::speech::FakeScript heard;
        heard.texts = {"Hello from OpenWhisper_CPP."};
        return heard;
    }();
    // resolve_parakeet finds the runtime and the model.
    bool engineInstalled = true;
    // What RuntimeSetup::ensureEngine answers.
    bool setupAnswer = true;
    // The crash log's last line.
    std::optional<fv::log::CrashInfo> lastCrash;
    bool takeCpuRuntime = false;
    // Component recovery throws this.
    std::optional<std::string> recoveryError;
    // Runs inside component recovery, before it throws.
    std::function<void()> duringRecovery;
    // Replaces the adoption the rig fakes (it records "adopt_missing" either way).
    std::function<fv::models::AdoptionReport(const fv::Json&, const fv::Paths&,
                                             const fv::CancelToken&)>
        adoptMissing;
    // The network and the credential store (both absent by default: no model runtimes, no API
    // keys); must outlive the rig.
    fv::http::Client* http = nullptr;
    fv::CredentialStore* credentials = nullptr;
    // Replaces the fake engine's factory (the cloud backend for "api", say); the fake engine
    // still answers "parakeet" through `fallback`.
    std::function<fv::speech::BackendFactory(fv::speech::BackendFactory fallback)> backends;
    // The download and install services' options (with `http`); the rig's host has no GPU.
    std::function<void(fv::ModelServiceOptions&)> modelServices;
    // Runs after the context is made and before the features run.
    std::function<void(fv::AppContext&)> beforeFeatures;
    // A Settings theme change goes through the controller's own path (the Application in the
    // context, when beforeFeatures provides one) instead of being recorded.
    bool realTheme = false;
    // What the update flow's Yes/No questions answer ("question:<title>" is recorded).
    bool questionAnswer = false;
    // Adjusts the controller's platform after the rig's fakes are in place.
    std::function<void(fv::ControllerPlatform&)> platform;
    // Links the Personalize feature (the AI cleanup, Learned rules, Dictionary, Snippets, Apps &
    // styles, Profiles and Commands pages and CleanupSettings), as the app does.
    bool personalize = false;
    bool walkthrough = false;
    fv::walkthrough::SnapPort* walkthroughSnap = nullptr;
    // Auto Mode's click watcher (empty: the platform's, which a test must not start).
    fv::WalkthroughFeature::ClickHook walkthroughClicks;
};

// The per-user folder (components, speech models) moved under the temporary data folder for the
// rig's life, so that nothing a test downloads, installs or scans is the owner's.
class ScopedPerUserFolder {
public:
    explicit ScopedPerUserFolder(const std::filesystem::path& root)
        : localAppData_(fv::testkit::get_env("LOCALAPPDATA")),
          xdgDataHome_(fv::testkit::get_env("XDG_DATA_HOME")) {
        const std::filesystem::path folder = root / "per-user";
        std::filesystem::create_directories(folder);
        const auto utf8 = folder.u8string();
        const std::string value(utf8.begin(), utf8.end());
        fv::testkit::set_env("LOCALAPPDATA", value);
        fv::testkit::set_env("XDG_DATA_HOME", value);
    }
    ~ScopedPerUserFolder() {
        fv::testkit::set_env("LOCALAPPDATA", localAppData_);
        fv::testkit::set_env("XDG_DATA_HOME", xdgDataHome_);
    }
    ScopedPerUserFolder(const ScopedPerUserFolder&) = delete;
    ScopedPerUserFolder& operator=(const ScopedPerUserFolder&) = delete;

private:
    std::optional<std::string> localAppData_;
    std::optional<std::string> xdgDataHome_;
};

// The controller and everything it needs, as Bootstrap builds them.
class ControllerRig {
public:
    explicit ControllerRig(RigOptions options = {})
        : options_(std::move(options)), perUser_(root_.path()),
          paths_(fv::Paths::resolve(fv::identity(), fv::system_env())),
          settings_(options_.settings), ui_(qApp), pool_("rig-pool", 4),
          io_(pool_, "io", 4, fv::ShutdownMode::CancelQueued),
          jobs_(pool_, "jobs", 2, fv::ShutdownMode::CancelQueued),
          persistence_(pool_, "persistence", 1, fv::ShutdownMode::Drain),
          audio_(std::make_shared<fv::audio::testing::FakeBackend>()),
          engine_(std::make_shared<fv::speech::FakeEngine>(options_.script)) {
        audio_->set_layout({{"MME"}}, {{"Microphone (Test)", 0, 1, 0, 44100.0}}, 0);
        speech_ = std::make_unique<fv::speech::SpeechService>(
            options_.backends ? options_.backends(engine_->factory()) : engine_->factory(), ui_);
        const std::filesystem::path databaseFile = paths_.user_file(fv::identity().database_file);
        database_ = std::make_unique<fv::Database>(
            offGui([databaseFile] { return fv::Database::open_or_create(databaseFile); }));

        fv::AppServices services;
        services.identity = &fv::identity();
        services.paths = &paths_;
        services.clock = &fv::system_clock();
        services.settings = &settings_;
        services.db = database_.get();
        services.audio = audio_.get();
        services.speech = speech_.get();
        services.ui = &ui_;
        services.jobs = &jobs_;
        services.persistence = &persistence_;
        services.io = &io_;
        services.shutdown = &plan_;
        services.cancel = &cancel_;
        services.activity = &activity_;
        services.http = options_.http;
        services.credentials = options_.credentials;
        context_ = std::make_unique<fv::AppContext>(services);
        if (options_.beforeFeatures) {
            options_.beforeFeatures(*context_);
        }

        fv::Strand* jobs = &jobs_;
        fv::Strand* persistence = &persistence_;
        fv::Database* database = database_.get();
        plan_.add(fv::ShutdownStep::Strands, "Error during executor shutdown",
                  [jobs] { jobs->shutdown(std::chrono::milliseconds(5000)); });
        plan_.add(fv::ShutdownStep::Persistence, {},
                  [persistence] { persistence->shutdown(std::chrono::milliseconds(30000)); });
        plan_.add(fv::ShutdownStep::Database, "Error closing database",
                  [database] { database->close(); });

        features_.push_back(std::make_unique<fv::ShellFeature>());
        features_.push_back(std::make_unique<fv::RecordFeature>());
        features_.push_back(std::make_unique<fv::HistoryFeature>());
        features_.push_back(std::make_unique<fv::OverlayFeature>());
        features_.push_back(std::make_unique<fv::SetupFeature>());
        features_.push_back(std::make_unique<fv::SettingsFeature>());
        features_.push_back(std::make_unique<fv::DownloadsFeature>());
        if (options_.personalize) {
            features_.push_back(std::make_unique<fv::PersonalizeFeature>());
        }
        if (options_.walkthrough) {
            features_.push_back(std::make_unique<fv::WalkthroughFeature>(
                options_.walkthroughSnap, options_.walkthroughClicks));
        }
        auto controller = std::make_unique<fv::ControllerFeature>(platform());
        controller_ = controller.get();
        features_.push_back(std::move(controller));
        fv::FeatureRegistry::run(features_, *context_);

        window_ = &context_->service<fv::MainWindow>();
        window_->availableScreenRect = [] {
            return QRect(0, 0, 1920, 1080);
        };
        window_->maxUsableHeight = [] {
            return 1040;
        };
        window_->completeSetup();
        fv::TrayEnvironment environment;
        environment.available = [] {
            return true;
        };
        environment.showIcon = [](QSystemTrayIcon&) {
        };
        tray_ = std::make_unique<fv::SystemTray>(*window_, environment);
        window_->setTrayAvailable(true);
        fv::connectShell(*window_, *tray_);
        lifecycle_ = context_->find<fv::ShellLifecycle>();
        if (lifecycle_ != nullptr && lifecycle_->trayCreated) {
            lifecycle_->trayCreated(*tray_);
        }

        const auto status = [this](std::string text) {
            std::lock_guard lock(mutex_);
            statuses_.push_back(std::move(text));
        };
        links_.push_back(app().status_update.connect(ui_, status));
        links_.push_back(runtime().status_update.connect(ui_, status));
        links_.push_back(hotkeys().status.connect(ui_, status));
    }

    ~ControllerRig() {
        links_.clear();
        plan_.run();
        io_.shutdown(std::chrono::milliseconds(2000));
        tray_.reset();
        features_.clear();
        context_.reset();
        speech_.reset();
        database_.reset();
    }

    ControllerRig(const ControllerRig&) = delete;
    ControllerRig& operator=(const ControllerRig&) = delete;

    // Bootstrap's "main window shown": the engine setup, then the first load and the
    // deferred database step.
    void mainWindowShown() {
        window_->show();
        if (lifecycle_ != nullptr && lifecycle_->mainWindowShown) {
            lifecycle_->mainWindowShown();
        }
    }
    // mainWindowShown(), then until the engine is loaded, warmed and settled.
    bool startEngine() {
        mainWindowShown();
        return waitUntil([this] {
            return speech_->readiness(fv::speech::Slot::Dictation).state ==
                       fv::speech::ReadyState::Ready &&
                   app().reload_machine().settled() && app().startup_database_ready();
        });
    }

    // A keypad key, as the hook stamps it now.
    void pressKeypad(std::string_view name) {
        for (const bool down : {true, false}) {
            fv::hotkeys::KeyEvent event;
            event.down = down;
            event.name = name;
            event.keypad = true;
            event.t = fv::hotkeys::monotonic_clock()();
            hook_->send(event);
        }
    }

    // Capture blocks for the open microphone: `seconds` of the tone fixture, looped.
    std::size_t speak(double seconds) {
        const auto pcm = fv::audio::testing::read_pcm(testkit_audio("tone-44k1-mono.wav"));
        auto streams = audio_->streams();
        if (streams.empty()) {
            return 0;
        }
        const auto stream = streams.back();
        const auto wanted = static_cast<std::size_t>(seconds * 44100);
        std::vector<std::int16_t> blocks;
        while (blocks.size() < wanted) {
            blocks.insert(blocks.end(), pcm.begin(), pcm.end());
        }
        blocks.resize(wanted);
        return stream->deliver_blocks(blocks);
    }

    static std::filesystem::path testkit_audio(const std::string& name) {
        return fv::testkit::source_root() / "tests" / "fixtures" / "audio" / name;
    }

    std::vector<std::string> statuses() const {
        std::lock_guard lock(mutex_);
        return statuses_;
    }
    bool hasStatus(const std::string& text) const {
        for (const auto& status : statuses()) {
            if (status == text) {
                return true;
            }
        }
        return false;
    }

    fv::AppContext& context() { return *context_; }
    fv::MainWindow& window() { return *window_; }
    fv::SystemTray& tray() { return *tray_; }
    fv::ControllerFeature& feature() { return *controller_; }
    fv::ApplicationController& app() { return context_->service<fv::ApplicationController>(); }
    fv::UiController& ui() { return *controller_->uiController(); }
    fv::dictation::TranscriptionRuntime& runtime() {
        return context_->service<fv::dictation::TranscriptionRuntime>();
    }
    fv::dictation::HotkeyRuntime& hotkeys() {
        return context_->service<fv::dictation::HotkeyRuntime>();
    }
    fv::WaveformOverlay& overlay() { return context_->service<fv::WaveformOverlay>(); }
    fv::QuickRecordTab* quickRecord() { return controller_->uiController()->quickRecord(); }
    fv::hotkeys::FakeKeyboardHook& hook() { return *hook_; }
    fv::hotkeys::FakeMouseHook& mouseHook() { return *mouseHook_; }
    fv::focus::FakeSyntheticKeys& keys() { return *keys_; }
    fv::audio::testing::FakeBackend& audio() { return *audio_; }
    fv::speech::FakeEngine& engine() { return *engine_; }
    fv::speech::SpeechService& speech() { return *speech_; }
    fv::Database& database() { return *database_; }
    fv::ActivityState& activity() { return activity_; }
    fv::ShutdownPlan& plan() { return plan_; }
    fv::MemorySettingsStore& settings() { return settings_; }
    const fv::Paths& paths() const { return paths_; }
    CallLog& calls() { return calls_; }
    fv::qt::QtUiExecutor& uiExecutor() { return ui_; }

private:
    fv::ControllerPlatform platform() {
        fv::ControllerPlatform made;
        made.keyboardHook = [this] {
            auto hook = std::make_unique<fv::hotkeys::FakeKeyboardHook>();
            hook_ = hook.get();
            return hook;
        };
        made.mouseHook = [this]() -> std::unique_ptr<fv::hotkeys::MouseHookSource> {
            auto hook = std::make_unique<fv::hotkeys::FakeMouseHook>();
            mouseHook_ = hook.get();
            return hook;
        };
        made.modelServices = [this](fv::ModelServiceOptions& services) {
            // A computer with no GPU, whose disks have room; nothing probes this one.
            fv::models::Host host;
            host.platform = std::string(fv::models::platform::kWinAmd64);
            services.store.host = host;
            services.store.free_bytes = [](const std::filesystem::path&) {
                return std::uint64_t{1} << 40;
            };
            if (options_.modelServices) {
                options_.modelServices(services);
            }
        };
        // The palette stays the test's: a theme change is only recorded, unless the test asked
        // for the controller's own theme path.
        if (!options_.realTheme) {
            made.applyTheme = [this](const QString& preference) {
                calls_.add("apply_theme:" + preference.toStdString());
                return true;
            };
        }
        made.applyFontScale = [this](int percent) {
            calls_.add("apply_font_scale:" + std::to_string(percent));
        };
        made.captureService = [](const fv::SettingsStore&) {
            return std::make_shared<NamedAppCapture>("notepad.exe", "Notepad");
        };
        made.syntheticKeys = [this] {
            auto keys = std::make_shared<fv::focus::FakeSyntheticKeys>();
            keys_ = keys.get();
            return keys;
        };
        made.recorder = [](fv::SettingsStore&, fv::audio::Backend& backend, const fv::Paths& paths,
                           fv::Executor&) {
            fv::RecorderOptions options;
            // A fake device delivers no blocks of its own: no stall or loss is declared while
            // a test is not delivering, and the post-roll ends at a short cap.
            options.timings.post_roll = std::chrono::milliseconds(50);
            options.timings.first_block_grace = std::chrono::minutes(5);
            options.timings.stall_after_blocks = std::chrono::minutes(5);
            return std::make_unique<fv::Recorder>(backend, paths, std::move(options));
        };
        const std::filesystem::path root = paths_.data_root();
        made.resolveEngine = [this, root](const fv::Json&, const fv::Paths&) {
            fv::models::ResolvedEngine engine;
            engine.model_key = "parakeet-v3";
            engine.model_label = "Parakeet TDT 0.6B v3";
            engine.device = "cpu";
            engine.requested_device = "auto";
            engine.runtime_id = "asr-nvidia-cpu";
            engine.runtime_dir = root / "components" / "asr-nvidia-cpu";
            if (options_.engineInstalled) {
                engine.model_path = root / "speech-models" / "parakeet-v3" / "model.gguf";
            } else {
                engine.missing_message = "Download Parakeet TDT 0.6B v3 in Downloads.";
                engine.missing_state = fv::models::EngineMissing::ModelMissing;
            }
            return engine;
        };
        made.adoptMissing = [this](const fv::Json& settings, const fv::Paths& paths,
                                   const fv::CancelToken& cancel) {
            calls_.add("adopt_missing");
            return options_.adoptMissing ? options_.adoptMissing(settings, paths, cancel)
                                         : fv::models::AdoptionReport{};
        };
        made.recoverComponents = [this](const fv::Paths&, fv::http::Client*) {
            calls_.add("recover_components");
            if (options_.duringRecovery) {
                options_.duringRecovery();
            }
            if (options_.recoveryError) {
                throw std::runtime_error(*options_.recoveryError);
            }
        };
        made.lastCrash = [this](const fv::Paths&) {
            return options_.lastCrash;
        };
        made.offerCpuRuntime = [this](QWidget*) {
            calls_.add("offer_cpu_runtime");
            return options_.takeCpuRuntime;
        };
        made.ensureEngine = [this](QWidget*) {
            calls_.add("ensure_engine");
            return options_.setupAnswer;
        };
        made.acquireComponent = [this](std::string_view id) {
            calls_.add("acquire:" + std::string(id));
        };
        // A development build with no update source: nothing reads the registry, checks for a
        // release, starts a process, exits the test or opens a browser.
        made.installProbe = [root] {
            fv::update::InstallProbe probe;
            probe.running_dir = root / "bin";
            return probe;
        };
        made.updateFeed = [] {
            return fv::update::FeedConfig{};
        };
        made.startDetached =
            [this](const std::filesystem::path& program, std::span<const std::string> args,
                   const std::filesystem::path&) -> std::expected<std::uint32_t, std::string> {
            std::string line = "start_detached:" + program.filename().string();
            for (const std::string& arg : args) {
                line += " " + arg;
            }
            calls_.add(line);
            return 4242;
        };
        made.exitApp = [this](int code) {
            calls_.add("exit:" + std::to_string(code));
        };
        made.armWatchdog = [this] {
            calls_.add("watchdog");
        };
        made.openUrl = [this](const QString& url) {
            calls_.add("open_url:" + url.toStdString());
        };
        made.question = [this](QWidget*, const QString& title, const QString& text) {
            calls_.add("question:" + title.toStdString() + ":" + text.toStdString());
            return options_.questionAnswer;
        };
        made.warning = [this](QWidget*, const QString& title, const QString& text) {
            calls_.add("warning:" + title.toStdString() + ":" + text.toStdString());
        };
        made.codeChanged = [] {
            return false;
        };
        made.execRestartOffer = [this](QMessageBox& box) {
            calls_.add("restart_offer:" + box.text().toStdString());
        };
        // No test reads an API key from this computer's environment.
        made.keyEnvironment = &noKeys_;
        if (options_.platform) {
            options_.platform(made);
        }
        return made;
    }

    RigOptions options_;
    fv::MapEnvReader noKeys_;
    fv::testkit::TempDataRoot root_;
    ScopedPerUserFolder perUser_;
    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::qt::QtUiExecutor ui_;
    fv::ThreadPool pool_;
    fv::Strand io_;
    fv::Strand jobs_;
    fv::Strand persistence_;
    std::shared_ptr<fv::audio::testing::FakeBackend> audio_;
    std::shared_ptr<fv::speech::FakeEngine> engine_;
    std::unique_ptr<fv::speech::SpeechService> speech_;
    std::unique_ptr<fv::Database> database_;
    fv::ActivityState activity_;
    fv::CancelRouter cancel_;
    fv::ShutdownPlan plan_;
    std::unique_ptr<fv::AppContext> context_;
    std::vector<std::unique_ptr<fv::Feature>> features_;
    fv::ControllerFeature* controller_ = nullptr;
    fv::MainWindow* window_ = nullptr;
    std::unique_ptr<fv::SystemTray> tray_;
    fv::ShellLifecycle* lifecycle_ = nullptr;
    fv::hotkeys::FakeKeyboardHook* hook_ = nullptr;
    fv::hotkeys::FakeMouseHook* mouseHook_ = nullptr;
    fv::focus::FakeSyntheticKeys* keys_ = nullptr;
    CallLog calls_;
    mutable std::mutex mutex_;
    std::vector<std::string> statuses_;
    std::vector<fv::Subscription> links_;
};

} // namespace fvtest
