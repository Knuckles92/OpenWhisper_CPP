// M3 end to end (section 11): the real feature graph, offscreen, under a temporary data folder and
// per-user folder, over the fake audio system, the fake speech engine, the fake keyboard hook, the
// in-memory credential store and a fake HTTP client. Nothing is started, installed or exited for
// real: the restart is appMain returning 73 (main() relaunches on it), the setup hand-off a fake
// start_detached and a recorded exit.
//   1. backup: create, inspect, "Restore and restart" (73), the next start applies the stage
//      before the settings open and lists the restored history, the replaced data under
//      .fiorivoce-restore\previous-*;
//   2. import: the guided setup's Welcome row opens the import window, "Import and restart" (73),
//      the next start has OpenWhisper's history, selected_model as parakeet, fv_import and the key;
//   3. update: the launch check (8 s on a fake clock) over the loopback feed lights the title bar
//      and the tray, the dialog downloads and verifies the setup, the hand-off starts it with
//      /FIORIVOCEUPDATE=1 and exits 0, and the About box names the release.
// The Backup page (M3.9) is not in this tree: the backup chain drives the coordinator the page
// would drive, through the context.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QPushButton>
#include <QTimer>

#include "backup/coordinator.h"
#include "backup/import.h"
#include "backup/restart.h"
#include "backup/restore.h"
#include "base/hash.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "base/testing/map_env_reader.h"
#include "controller_support.h"
#include "credentials/testing/memory_backend.h"
#include "engine/platform.h"
#include "engine/version.h"
#include "history/manager.h"
#include "http/testing/fake_http_client.h"
#include "settings/keys.h"
#include "sql/db.h"
#include "ui_controller/update_presenter.h"
#include "ui_setup/app_update_dialog.h"
#include "ui_setup/guided_setup_dialog.h"
#include "ui_setup/guided_setup_offer.h"
#include "ui_setup/openwhisper_import.h"
#include "ui_setup/openwhisper_import_dialog.h"
#include "ui_shell/application.h"
#include "ui_shell/single_instance.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/update_notice.h"
#include "ui_widgets/wrapped_label.h"
#include "update/download.h"
#include "update/release_feed.h"
#include "update/runtime.h"
#include "update/testing/fake_feed.h"

using fvtest::CallLog;
using fvtest::ControllerRig;
using fvtest::offGui;
using fvtest::RigOptions;
using fvtest::waitUntil;
using namespace std::chrono_literals;

namespace fs = std::filesystem;

namespace {

// Ends a run whose exit never came, so a broken chain fails instead of hanging.
constexpr int kStuckExit = 99;

// The archive functions with the import's keys copied between in-memory stores over the test's
// keyring: OpenWhisper's credential service to FioriVoce's, never the computer's own.
class MemoryKeysService final : public fv::backup::BackupService {
public:
    explicit MemoryKeysService(std::shared_ptr<fv::MemoryCredentialBackend> keyring)
        : keyring_(std::move(keyring)) {}

    fv::backup::BackupInfo create_backup(const fs::path& destination,
                                         const fv::backup::CreateOptions& options) override {
        return fv::backup::real_backup_service().create_backup(destination, options);
    }
    fv::backup::BackupInfo inspect_backup(const fs::path& archive) override {
        return fv::backup::real_backup_service().inspect_backup(archive);
    }
    fs::path prepare_restore(const fs::path& archive, const fs::path& root) override {
        return fv::backup::real_backup_service().prepare_restore(archive, root);
    }
    std::optional<fs::path> previous_data_dir(const fs::path& root) override {
        return fv::backup::real_backup_service().previous_data_dir(root);
    }
    fv::backup::ImportResult prepare_import(const fs::path& source, const fs::path& root,
                                            const fv::backup::ImportOptions& options) override {
        fv::CredentialStore from(keyring_, fv::legacy_identity());
        fv::CredentialStore to(keyring_, fv::identity());
        fv::backup::ImportOptions own = options;
        own.credentials_from = &from;
        own.credentials_to = &to;
        return fv::backup::stage_from_data_root(fv::legacy_identity(), source, fv::identity(), root,
                                                own);
    }

private:
    std::shared_ptr<fv::MemoryCredentialBackend> keyring_;
};

// fv::appMain over the test's data folder, as main() runs it, once per start: the same keyring,
// engine and backup service each time, so a second start sees what the first left.
class M3App {
public:
    using Run = std::function<int(fv::MainWindow&)>;

    M3App()
        : dataDir_(useDataDir(root_.path() / "data")), perUser_(root_.path()),
          keyring_(std::make_shared<fv::MemoryCredentialBackend>()), service_(keyring_),
          engine_(std::make_shared<fv::speech::FakeEngine>()),
          http_(std::make_shared<fv::http::FakeHttpClient>()),
          legacyDataDir_(fv::testkit::get_env(legacyDataDirVariable())) {
        // OpenWhisper's data is looked for only in the test's per-user folder.
        fv::testkit::set_env(legacyDataDirVariable(), std::nullopt);
    }
    ~M3App() { fv::testkit::set_env(legacyDataDirVariable(), legacyDataDir_); }
    M3App(const M3App&) = delete;
    M3App& operator=(const M3App&) = delete;

    // The data folder beside the per-user folder, never around it: an import refuses a source
    // inside the data folder it restores into, as OpenWhisper's real folders never are.
    static fs::path useDataDir(const fs::path& folder) {
        fs::create_directories(folder);
        const auto utf8 = folder.u8string();
        fv::testkit::set_env("FIORIVOCE_DATA_DIR", std::string(utf8.begin(), utf8.end()));
        return folder;
    }

    static std::string legacyDataDirVariable() {
        return std::string(fv::legacy_identity().env_prefix) + "DATA_DIR";
    }

    const fs::path& root() const { return root_.path(); }
    fv::MemoryCredentialBackend& keyring() { return *keyring_; }
    CallLog& calls() { return calls_; }
    const std::vector<std::string>& errors() const { return errors_; }

    // One start: run() gets the shown main window and returns the event loop's code.
    int start(const Run& run) {
        int argc = 1;
        char program[] = "fiorivoce";
        char* argv[] = {program, nullptr};
        return fv::appMain(argc, argv, hooks(run));
    }

private:
    fv::BootstrapHooks hooks(const Run& run) {
        fv::BootstrapHooks made;
        made.arguments = std::vector<std::string>{"fiorivoce"};
        made.features = [this] {
            std::vector<std::unique_ptr<fv::Feature>> features;
            features.push_back(std::make_unique<fv::ShellFeature>());
            features.push_back(std::make_unique<fv::RecordFeature>());
            features.push_back(std::make_unique<fv::HistoryFeature>());
            features.push_back(std::make_unique<fv::OverlayFeature>());
            features.push_back(std::make_unique<fv::SetupFeature>());
            features.push_back(std::make_unique<fv::SettingsFeature>());
            features.push_back(std::make_unique<fv::DownloadsFeature>());
            features.push_back(std::make_unique<fv::ControllerFeature>(platform()));
            return features;
        };
        made.setupLogging = [](const fv::Paths&) {
            return std::shared_ptr<void>();
        };
        const fs::path locks = root_.path() / "run";
        made.acquireInstance = [locks](const fv::Paths& paths) {
            fv::SingleInstanceOptions options;
            options.lockFolder = locks;
            options.handoff = 200ms;
            return fv::SingleInstance::acquire(paths.data_root(), fv::identity(), options);
        };
        made.credentialBackend = [keyring = keyring_] {
            return keyring;
        };
        made.tray.available = [] {
            return true;
        };
        made.tray.showIcon = [](QSystemTrayIcon&) {
        };
        made.processEvents = [] {
            QApplication::processEvents();
        };
        made.run = [run](fv::Application&, fv::MainWindow& window) {
            return run(window);
        };
        made.errorBox = [this](const std::string& message) {
            errors_.push_back(message);
        };
        made.audioBackend = [] {
            auto backend = std::make_shared<fv::audio::testing::FakeBackend>();
            backend->set_layout({{"MME"}}, {{"Microphone (Test)", 0, 1, 0, 44100.0}}, 0);
            return backend;
        };
        made.speechBackends = [engine = engine_] {
            return engine->factory();
        };
        made.httpClient = [http = http_]() -> std::shared_ptr<fv::http::Client> {
            return http;
        };
        return made;
    }

    // The rig's fakes (controller_support.h), for a graph Bootstrap builds.
    fv::ControllerPlatform platform() {
        fv::ControllerPlatform made;
        made.keyboardHook = [] {
            return std::make_unique<fv::hotkeys::FakeKeyboardHook>();
        };
        made.mouseHook = []() -> std::unique_ptr<fv::hotkeys::MouseHookSource> {
            return std::make_unique<fv::hotkeys::FakeMouseHook>();
        };
        made.captureService = [](const fv::SettingsStore&) {
            return std::make_shared<fvtest::NamedAppCapture>("notepad.exe", "Notepad");
        };
        made.syntheticKeys = [] {
            return std::make_shared<fv::focus::FakeSyntheticKeys>();
        };
        made.recorder = [](fv::SettingsStore&, fv::audio::Backend& backend, const fv::Paths& paths,
                           fv::Executor&) {
            fv::RecorderOptions options;
            options.timings.post_roll = 50ms;
            options.timings.first_block_grace = 5min;
            options.timings.stall_after_blocks = 5min;
            return std::make_unique<fv::Recorder>(backend, paths, std::move(options));
        };
        made.resolveEngine = [](const fv::Json&, const fv::Paths& paths) {
            fv::models::ResolvedEngine engine;
            engine.model_key = "parakeet-v3";
            engine.model_label = "Parakeet TDT 0.6B v3";
            engine.device = "cpu";
            engine.requested_device = "auto";
            engine.runtime_id = "asr-nvidia-cpu";
            engine.runtime_dir = paths.data_root() / "components" / "asr-nvidia-cpu";
            engine.model_path = paths.data_root() / "speech-models" / "parakeet-v3" / "model.gguf";
            return engine;
        };
        made.adoptMissing = [](const fv::Json&, const fv::Paths&, const fv::CancelToken&) {
            return fv::models::AdoptionReport{};
        };
        made.recoverComponents = [](const fv::Paths&, fv::http::Client*) {
        };
        made.lastCrash = [](const fv::Paths&) {
            return std::optional<fv::log::CrashInfo>();
        };
        made.offerCpuRuntime = [](QWidget*) {
            return false;
        };
        made.ensureEngine = [](QWidget*) {
            return true;
        };
        made.acquireComponent = [](std::string_view) {
        };
        made.modelServices = [](fv::ModelServiceOptions& services) {
            fv::models::Host host;
            host.platform = std::string(fv::models::platform::kWinAmd64);
            services.store.host = host;
            services.store.free_bytes = [](const fs::path&) {
                return std::uint64_t{1} << 40;
            };
        };
        made.applyTheme = [](const QString&) {
            return true;
        };
        made.applyFontScale = [](int) {
        };
        made.installProbe = [root = root_.path()] {
            fv::update::InstallProbe probe;
            probe.running_dir = root / "bin";
            return probe;
        };
        made.updateFeed = [] {
            return fv::update::FeedConfig{};
        };
        made.startDetached = [this](const fs::path& program, std::span<const std::string>,
                                    const fs::path&) -> std::expected<std::uint32_t, std::string> {
            calls_.add("start_detached:" + program.filename().string());
            return 1;
        };
        made.armWatchdog = [this] {
            calls_.add("watchdog");
        };
        made.openUrl = [this](const QString& url) {
            calls_.add("open_url:" + url.toStdString());
        };
        made.question = [](QWidget*, const QString&, const QString&) {
            return false;
        };
        made.warning = [this](QWidget*, const QString& title, const QString& text) {
            calls_.add("warning:" + title.toStdString() + ":" + text.toStdString());
        };
        made.codeChanged = [] {
            return false;
        };
        made.execRestartOffer = [](QMessageBox&) {
        };
        // The restarts are QCoreApplication::exit(73), the real default: the event loop the
        // run hook started returns it, and appMain with it.
        made.backupService = &service_;
        return made;
    }

    fv::testkit::TempDataRoot root_;
    fs::path dataDir_;
    fvtest::ScopedPerUserFolder perUser_;
    std::shared_ptr<fv::MemoryCredentialBackend> keyring_;
    MemoryKeysService service_;
    std::shared_ptr<fv::speech::FakeEngine> engine_;
    std::shared_ptr<fv::http::FakeHttpClient> http_;
    std::optional<std::string> legacyDataDir_;
    CallLog calls_;
    std::vector<std::string> errors_;
};

// Waits for the startup database step and the recovery after it: a backup is refused before.
bool startupSettled(fv::AppContext& context) {
    auto& app = context.service<fv::ApplicationController>();
    return waitUntil(
        [&] {
            return app.startup_database_ready() && !context.activity().snapshot().recovery_running;
        },
        30s);
}

std::vector<std::string> historyTexts(fv::AppContext& context) {
    auto& history = context.service<fv::HistoryManager>();
    return offGui([&history] {
        std::vector<std::string> texts;
        for (const auto& entry : history.get_history()) {
            texts.push_back(entry.text);
        }
        return texts;
    });
}

bool has(const std::vector<std::string>& texts, const std::string& text) {
    return std::ranges::find(texts, text) != texts.end();
}

// The fixture's OpenWhisper data folder copied to `target`, its database's paths, which start
// with a placeholder root, made paths below `target` as if OpenWhisper had run there (as
// tests/backup/test_import.cpp installs it). Off the GUI thread: SQLite blocks.
void installOpenWhisperFixture(const fs::path& target) {
    const fs::path fixtures = fv::testkit::source_root() / "tests" / "fixtures" / "import";
    fs::create_directories(target.parent_path());
    fs::copy(fixtures / "openwhisper", target, fs::copy_options::recursive);
    std::ifstream in(fixtures / "fixture.json", std::ios::binary);
    const std::string prefix = fv::Json::parse(in).at("placeholder").get<std::string>();
    const auto rebased = [&prefix, &target](const std::string& text) {
        if (!text.starts_with(prefix)) {
            return text;
        }
        fs::path path = target;
        std::string rest = text.substr(prefix.size());
        for (std::size_t at = 0; at < rest.size();) {
            const std::size_t next = rest.find('\\', at);
            const std::string part =
                rest.substr(at, next == std::string::npos ? std::string::npos : next - at);
            if (!part.empty()) {
                path /= fv::path_from_utf8(part);
            }
            if (next == std::string::npos) {
                break;
            }
            at = next + 1;
        }
        return fv::path_to_utf8(path);
    };
    offGui([&] {
        fv::sql::Connection connection =
            fv::sql::Connection::open(target / "openwhisper.db", fv::sql::OpenMode::ReadWrite);
        for (const auto& [table, column] :
             {std::pair<std::string, std::string>{"meeting_sessions", "spool_dir"},
              {"meeting_audio_chunks", "file_path"},
              {"transcription_history", "audio_file"}}) {
            std::vector<std::pair<fv::sql::Value, std::string>> rows;
            {
                auto select = connection.prepare("SELECT rowid, " + column + " FROM " + table +
                                                 " WHERE " + column + " IS NOT NULL");
                while (select.step()) {
                    rows.emplace_back(select.column(0), std::get<std::string>(select.column(1)));
                }
            }
            for (const auto& [rowid, text] : rows) {
                auto update =
                    connection.prepare("UPDATE " + table + " SET " + column + "=? WHERE rowid=?");
                update.bind(1, rebased(text)).bind(2, rowid);
                update.step();
            }
        }
        connection.close();
        return true;
    });
}

// The event loop, with a way out if the restart it waits for never comes.
int runUntilExit() {
    QTimer::singleShot(60'000, [] { QCoreApplication::exit(kStuckExit); });
    return QApplication::exec();
}

} // namespace

TEST_CASE("M3 end to end: back up, restore and restart, and open on the restored data",
          "[e2e][widget]") {
    M3App app;
    const fs::path archive = app.root() / "bundles" / "FioriVoce-backup.fvbackup";
    fs::create_directories(archive.parent_path());
    std::optional<fv::backup::BackupInfo> created;
    std::optional<fv::backup::BackupInfo> inspected;

    const int first = app.start([&](fv::MainWindow& window) {
        fv::AppContext& context = window.context();
        if (!startupSettled(context)) {
            FAIL_CHECK("the startup database step never finished");
            return 1;
        }
        offGui([&context] {
            fv::AddEntry entry;
            entry.text = "kept by the backup";
            entry.model = "parakeet";
            return context.service<fv::HistoryManager>().add_entry(entry).has_value();
        });
        auto& coordinator = context.service<fv::backup::BackupCoordinator>();
        std::string error = "pending";
        auto backedUp = coordinator.backup_finished.connect(
            context.ui(), [&](std::optional<fv::backup::BackupInfo> info, std::string failure) {
                created = std::move(info);
                error = std::move(failure);
            });
        coordinator.create_backup(archive);
        if (!waitUntil([&error] { return error != "pending"; }, 60s) || !error.empty()) {
            FAIL_CHECK("the backup failed: " << error);
            return 1;
        }
        error = "pending";
        auto looked = coordinator.inspection_finished.connect(
            context.ui(), [&](std::optional<fv::backup::BackupInfo> info, std::string failure) {
                inspected = std::move(info);
                error = std::move(failure);
            });
        coordinator.inspect_backup(archive);
        if (!waitUntil([&error] { return error != "pending"; }, 60s) || !error.empty()) {
            FAIL_CHECK("the inspection failed: " << error);
            return 1;
        }
        // The data changes after the backup; the restore must bring the entry back.
        offGui([&context] {
            context.service<fv::HistoryManager>().clear_history();
            return true;
        });
        // "Restore and restart": the stage is published on the worker and the app exits with 73
        // from inside the event loop, as from the Backup page.
        coordinator.prepare_restore(archive);
        return runUntilExit();
    });

    CHECK(first == fv::backup::restart_exit_code);
    REQUIRE(created.has_value());
    CHECK(created->app_version == fv::kVersion);
    REQUIRE(inspected.has_value());
    CHECK(inspected->app_version == fv::kVersion);
    CHECK(app.errors().empty());

    std::vector<std::string> restored;
    std::optional<fs::path> previous;
    const int second = app.start([&](fv::MainWindow& window) {
        fv::AppContext& context = window.context();
        if (!startupSettled(context)) {
            FAIL_CHECK("the startup database step never finished");
            return 1;
        }
        restored = historyTexts(context);
        previous = fv::backup::previous_data_dir(context.paths().data_root());
        return 0;
    });

    CHECK(second == 0);
    CHECK(has(restored, "kept by the backup"));
    REQUIRE(previous.has_value());
    CHECK(previous->filename().string().starts_with("previous-"));
    CHECK(previous->parent_path().filename() == fs::path(std::string(fv::identity().restore_dir)));
    CHECK(fs::exists(*previous / std::string(fv::identity().database_file)));
    CHECK(app.errors().empty());
}

TEST_CASE("M3 end to end: import OpenWhisper's data from the guided setup and restart",
          "[e2e][widget]") {
    M3App app;
    // OpenWhisper's per-user data folder: the fixture, where the import looks for it.
    const fs::path openWhisper = fs::path(*fv::testkit::get_env("LOCALAPPDATA")) /
                                 std::string(fv::legacy_identity().data_dir_name);
    installOpenWhisperFixture(openWhisper);

    app.keyring().set(fv::legacy_identity().credential_service, "OPENAI_API_KEY",
                      "sk-import-end-to-end");

    bool importOffered = false;
    std::string importStatus;
    const int first = app.start([&](fv::MainWindow& window) {
        fv::AppContext& context = window.context();
        if (!startupSettled(context)) {
            FAIL_CHECK("the startup database step never finished");
            return 1;
        }
        // A new install's first show offers the guided setup on its Welcome page, which looks
        // for OpenWhisper's data and offers the import.
        auto& guided = context.service<fv::GuidedSetup>();
        if (!waitUntil(
                [&guided] {
                    return guided.dialog() != nullptr && guided.dialog()->isVisible() &&
                           guided.dialog()->importRow() != nullptr &&
                           guided.dialog()->importRow()->isVisible();
                },
                20s)) {
            FAIL_CHECK("the Welcome page never offered the import");
            return 1;
        }
        importOffered = true;
        guided.dialog()->importButton()->click();
        auto& import = context.service<fv::OpenWhisperImport>();
        if (!waitUntil(
                [&import] {
                    return import.dialog() != nullptr && import.dialog()->isVisible() &&
                           import.dialog()->importButton()->isEnabled();
                },
                20s)) {
            FAIL_CHECK("the import window never found the data");
            return 1;
        }
        import.dialog()->copyKeysBox()->setChecked(true);
        import.dialog()->importButton()->click();
        const int code = runUntilExit();
        if (import.dialog() != nullptr) {
            importStatus = import.dialog()->statusLabel()->text().toStdString();
        }
        return code;
    });
    INFO("the import window said: " << importStatus);

    CHECK(importOffered);
    CHECK(first == fv::backup::restart_exit_code);

    std::vector<std::string> history;
    fv::Json selectedModel;
    fv::Json record;
    std::optional<std::string> key;
    const int second = app.start([&](fv::MainWindow& window) {
        fv::AppContext& context = window.context();
        if (!startupSettled(context)) {
            FAIL_CHECK("the startup database step never finished");
            return 1;
        }
        history = historyTexts(context);
        selectedModel = context.settings().value("selected_model");
        record = context.settings().value(std::string(fv::keys::fv_import.key()));
        key = offGui([&context] { return context.credentials().get("OPENAI_API_KEY"); });
        return 0;
    });

    CHECK(second == 0);
    CHECK(has(history, "relative audio"));
    CHECK(has(history, "absolute audio"));
    CHECK(has(history, "no audio"));
    CHECK(selectedModel == "parakeet");
    REQUIRE(record.is_object());
    CHECK(record.contains("at"));
    CHECK(record.value("version", "") == fv::kVersion);
    CHECK(key == "sk-import-end-to-end");
    CHECK(app.keyring().get(fv::identity().credential_service, "OPENAI_API_KEY") ==
          "sk-import-end-to-end");
    CHECK(app.errors().empty());
}

TEST_CASE("M3 end to end: the launch check finds a release and the setup hand-off exits 0",
          "[e2e][widget]") {
    namespace update = fv::update;
    const std::string feedUrl = "http://127.0.0.1:8765/releases/latest";
    const std::string setupName = update::setup_asset_name("0.2.0");
    const std::string setupUrl = "http://127.0.0.1:8765/download/v0.2.0/" + setupName;
    const std::string setupBytes = "a verified FioriVoce 0.2.0 setup";

    update::testing::FakeFeed feed;
    update::testing::FakeFeed::ReleaseSpec spec;
    spec.version = "0.2.0";
    spec.url = setupUrl;
    spec.size = static_cast<std::int64_t>(setupBytes.size());
    spec.digest = fv::sha256_hex(setupBytes);
    feed.routes[feedUrl] = update::testing::FakeFeed::json(feed.release(spec));
    fv::http::ScriptedResponse download;
    download.body = setupBytes;
    download.headers = {{"Content-Length", std::to_string(setupBytes.size())}};
    feed.routes[setupUrl] = download;

    fv::FakeClock clock;
    fv::ManualExecutor updaterUi(clock);
    fs::path installDir;
    QString aboutHtml;
    RigOptions options;
    options.http = &feed;
    options.platform = [&](fv::ControllerPlatform& platform) {
        // FIORIVOCE_UPDATE_FEED_URL on the loopback feed, read as the app reads it.
        platform.updateFeed = [&feedUrl] {
            const fv::MapEnvReader env{{update::update_feed_env(), feedUrl}};
            return update::feed_config(env);
        };
        platform.installProbe = [&installDir] {
            update::InstallProbe probe;
            probe.registered = true;
            probe.install_dir = installDir;
            probe.running_dir = installDir;
            return probe;
        };
        platform.updaterUi = &updaterUi;
        platform.updaterClock = &clock;
        // The product's platform, whatever system the test runs on: a setup is offered on Windows.
        platform.updatePlatform = "win32";
        platform.aboutBox = [&aboutHtml](QWidget*, const QString&, const QString& html) {
            aboutHtml = html;
        };
    };
    options.beforeFeatures = [&installDir](fv::AppContext& context) {
        installDir = context.paths().local_app_dir() / "Programs" / "FioriVoce";
        fs::create_directories(installDir);
    };
    ControllerRig rig(options);
    update::UpdaterRuntime* runtime = rig.feature().updater();
    REQUIRE(runtime != nullptr);
    fv::UpdatePresenter& presenter = *rig.feature().updatePresenter();

    REQUIRE(rig.startEngine());
    // The launch check waits out startup: 8 s.
    CHECK(runtime->check_timer_delay() == update::kUpdateCheckDelay);
    updaterUi.advance(update::kUpdateCheckDelay - 1ms);
    CHECK_FALSE(has(feed.urls_requested(), feedUrl));
    updaterUi.advance(1ms);
    REQUIRE(waitUntil(
        [&] {
            updaterUi.run_all();
            return presenter.lastResult().has_value();
        },
        30s));
    CHECK(presenter.lastResult()->status == update::UpdateStatus::UpdateAvailable);
    CHECK(rig.window().updateNotice()->text() == QStringLiteral("Update to 0.2.0"));
    CHECK_FALSE(rig.window().updateNotice()->isHidden());
    QAction* trayItem = rig.context().menus().action(fv::tray_id::update);
    CHECK(trayItem->isVisible());
    CHECK(trayItem->text() == QStringLiteral("Update to 0.2.0…"));
    CHECK(presenter.aboutStatusLine() == QStringLiteral("Update available: 0.2.0"));

    // The notice opens the dialog; "Download and install" fetches and verifies the setup.
    Q_EMIT rig.window().updateNoticeRequested();
    fv::AppUpdateDialog* dialog = presenter.dialog();
    REQUIRE(dialog != nullptr);
    REQUIRE(dialog->primaryButton()->text() == QStringLiteral("Download and install"));
    dialog->primaryButton()->click();
    REQUIRE(waitUntil(
        [&] {
            updaterUi.run_all();
            return rig.calls().has("exit:0");
        },
        30s));

    const fs::path setup = update::updates_root(rig.paths()) / setupName;
    CHECK(fs::exists(setup));
    CHECK(fv::sha256_hex(offGui([&setup] {
              std::ifstream in(setup, std::ios::binary);
              return std::string(std::istreambuf_iterator<char>(in),
                                 std::istreambuf_iterator<char>());
          })) == fv::sha256_hex(setupBytes));
    std::string launch;
    for (const std::string& call : rig.calls().all()) {
        if (call.starts_with("start_detached:")) {
            launch = call;
        }
    }
    CHECK(launch.starts_with("start_detached:" + setupName));
    CHECK(launch.find(" /FIORIVOCEUPDATE=1") != std::string::npos);
    CHECK(launch.find(" /SILENT") != std::string::npos);
    CHECK(launch.find(" /CURRENTUSER") != std::string::npos);
    CHECK(rig.calls().has("watchdog"));
    CHECK_FALSE(dialog->busy());

    // Help > About names the release the check found.
    Q_EMIT rig.window().aboutRequested();
    CHECK(aboutHtml.contains(QStringLiteral("<p>Update status: Update available: 0.2.0</p>")));
    CHECK(aboutHtml.contains(QStringLiteral("Install: Windows installer</p>")));
}
