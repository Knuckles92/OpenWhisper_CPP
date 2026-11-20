// The startup sequence, its cleanup, the hand-off to a running UI and the data lease. Ported from
// tests/test_bootstrap.py and tests/test_backup_startup.py; Bootstrap's hooks stand where
// Python's tests patched setup_logging, acquire_ui_instance and the runtime components.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <QApplication>
#include <QThread>

#include "agentapi/mcp_runtime.h"

#include "appcore/shutdown_plan.h"
#include "appkit/app_context.h"
#include "appkit/feature.h"
#include "audio/testing/fake_backend.h"
#include "backup/create.h"
#include "backup/restore.h"
#include "base/data_lease.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/metrics.h"
#include "base/paths.h"
#include "credentials/testing/memory_backend.h"
#include "credentials/validate.h"
#include "db/database.h"
#include "process/process_env.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "shell_fixture.h"
#include "sql/db.h"
#include "testkit/testkit.h"
#include "ui_shell/application.h"
#include "ui_shell/bootstrap.h"
#include "ui_shell/loading_screen.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_shell/single_instance.h"
#include "ui_shell/system_tray.h"
#include <QTimer>
#include <atomic>
#include <catch2/generators/catch_generators.hpp>

using namespace std::chrono_literals;

namespace {

class CapturedLog final : public fv::log::Sink {
public:
    CapturedLog() = default;
    void write(const fv::log::Record& record) override {
        const std::lock_guard lock(mutex);
        lines.push_back(std::string(record.logger) + ": " + std::string(record.message));
    }
    bool has(const std::string& line) {
        const std::lock_guard lock(mutex);
        return std::ranges::find(lines, line) != lines.end();
    }
    bool has_prefix(const std::string& prefix) {
        const std::lock_guard lock(mutex);
        return std::ranges::any_of(
            lines, [&](const std::string& line) { return line.starts_with(prefix); });
    }
    std::mutex mutex;
    std::vector<std::string> lines;
};

// Installs a log sink for one test case.
struct LogCapture {
    std::shared_ptr<CapturedLog> log = std::make_shared<CapturedLog>();
    LogCapture() { fv::log::add_sink(log); }
    ~LogCapture() { fv::log::remove_sink(log); }
};

// A backend that accepted the probe and never answered.
class SilentKeyring final : public fv::MemoryCredentialBackend {
public:
    fv::CredentialStatus probe() override {
        return {false, "silent store", std::string(fv::kKeyringNoResponse)};
    }
};

// The hooks a test starts from: no logging set up, the shell feature alone, an instance whose
// lock lives in the test's data folder, a fake tray, the in-memory credential store, and an
// event loop that returns 123 at once. Each step lands in `steps`.
struct Harness {
    fv::testkit::TempDataRoot root;
    std::vector<std::string> steps;
    std::vector<std::string> errors;
    int argc = 1;
    char program[10] = "openwhisper-cpp";
    char* argv[2] = {program, nullptr};

    fv::BootstrapHooks hooks() {
        fv::BootstrapHooks made;
        made.features = [] {
            std::vector<std::unique_ptr<fv::Feature>> features;
            features.push_back(std::make_unique<fv::ShellFeature>());
            return features;
        };
        made.setupLogging = [](const fv::Paths&) {
            return std::shared_ptr<void>();
        };
        const std::filesystem::path locks = root.path() / "run";
        made.acquireInstance = [locks](const fv::Paths& paths) {
            fv::SingleInstanceOptions options;
            options.lockFolder = locks;
            options.handoff = 200ms;
            return fv::SingleInstance::acquire(paths.data_root(), fv::identity(), options);
        };
        made.credentialBackend = [] {
            return std::make_shared<fv::MemoryCredentialBackend>();
        };
        made.tray.available = [] {
            return true;
        };
        made.tray.showIcon = [](QSystemTrayIcon&) {
        };
        made.processEvents = [] {
            QApplication::processEvents();
        };
        made.run = [](fv::Application&, fv::MainWindow&) {
            return 123;
        };
        made.errorBox = [this](const std::string& message) {
            errors.push_back(message);
        };
        made.step = [this](std::string_view step) {
            steps.emplace_back(step);
        };
        made.arguments = std::vector<std::string>{"openwhisper-cpp"};
        // No audio system is opened by a startup test.
        made.audioBackend = [] {
            return std::make_shared<fv::audio::testing::FakeBackend>();
        };
        return made;
    }

    std::ptrdiff_t index(const std::string& step) const {
        const auto it = std::ranges::find(steps, step);
        return it == steps.end() ? -1 : it - steps.begin();
    }
    bool has(const std::string& step) const { return index(step) >= 0; }
};

fv::Paths paths() {
    return fv::Paths::resolve(fv::identity(), fv::system_env());
}

} // namespace

// from tests/test_bootstrap.py::TestBootstrap::test_main_runs_startup_flow_and_cleans_up_controller
TEST_CASE("main runs startup flow and cleans up controller", "[test_bootstrap][widget]") {
    Harness harness;
    LogCapture capture;
    std::vector<double> startupToWindow;
    fv::metrics::set_sink([&startupToWindow](std::string_view name, double value) {
        if (name == "startup_to_window_s") {
            startupToWindow.push_back(value);
        }
    });
    fv::Bootstrap bootstrap(harness.hooks());
    const int result = bootstrap.run(harness.argc, harness.argv);
    fv::metrics::set_sink({});

    CHECK(result == 123);
    REQUIRE(harness.has("loading_screen_shown"));
    REQUIRE(harness.has("features"));
    // The splash is up and painted before the features and the window are built.
    CHECK(harness.index("loading_screen_shown") < harness.index("process_events"));
    CHECK(harness.index("process_events") < harness.index("features"));
    CHECK(harness.index("features") < harness.index("main_window_created"));
    CHECK(harness.index("loading_screen_destroyed") < harness.index("main_window_shown"));
    CHECK(harness.index("main_window_shown") < harness.index("run"));
    // The instance goes first, so a new launch can take over during cleanup.
    CHECK(harness.index("run") < harness.index("instance_released"));
    CHECK(harness.index("instance_released") < harness.index("shutdown"));
    CHECK(std::ranges::count(harness.steps, "process_events") == 4);

    CHECK(startupToWindow.size() == 1);
    for (const char* line :
         {"root: Starting OpenWhisper_CPP", "root: Application initialization complete",
          "root: Starting event loop", "root: Application shutdown complete",
          "ui_qt.startup_profiler: Startup timing summary:"}) {
        CHECK(capture.log->has(line));
    }
    CHECK(std::ranges::count(capture.log->lines, "root: " + std::string(60, '=')) == 4);

    // The settings migrations ran: a new install keeps the engine it never chose.
    const fv::SettingsStore settings(paths().user_file(fv::identity().settings_file));
    CHECK(settings.value("selected_model") == "parakeet");
    CHECK(settings.value("app_style_tones").is_object());
}

// from
// tests/test_bootstrap.py::TestBootstrap::test_main_cleans_up_loading_screen_and_controller_on_run_error
TEST_CASE("main cleans up loading screen and controller on run error", "[test_bootstrap][widget]") {
    Harness harness;
    LogCapture capture;
    fv::BootstrapHooks hooks = harness.hooks();
    hooks.run = [](fv::Application&, fv::MainWindow&) -> int {
        throw std::runtime_error("boom");
    };
    fv::Bootstrap bootstrap(std::move(hooks));
    CHECK_THROWS_WITH(bootstrap.run(harness.argc, harness.argv), "boom");
    CHECK(harness.has("instance_released"));
    CHECK(harness.has("shutdown"));
    CHECK(capture.log->has("root: Application startup failed"));
    CHECK(capture.log->has("root: Application shutdown complete"));
    // Released: the next launch is primary again.
    fv::SingleInstanceOptions options;
    options.lockFolder = harness.root.path() / "run";
    CHECK(fv::SingleInstance::acquire(paths().data_root(), fv::identity(), options) != nullptr);
}

TEST_CASE("a startup failure before the window still cleans up the splash", "[widget]") {
    Harness harness;
    LogCapture capture;
    fv::BootstrapHooks hooks = harness.hooks();
    hooks.features = []() -> std::vector<std::unique_ptr<fv::Feature>> {
        throw std::runtime_error("no features");
    };
    fv::Bootstrap bootstrap(std::move(hooks));
    CHECK_THROWS_WITH(bootstrap.run(harness.argc, harness.argv), "no features");
    CHECK(harness.has("loading_screen_shown"));
    CHECK_FALSE(harness.has("main_window_shown"));
    CHECK(harness.has("shutdown"));
    CHECK(capture.log->has("ui_qt.startup_profiler: Startup timing summary:"));
    CHECK(capture.log->has("root: Application startup failed"));
}

// from
// tests/test_bootstrap.py::TestBootstrap::test_main_tells_the_user_when_the_keyring_does_not_answer
TEST_CASE("main tells the user when the keyring does not answer", "[test_bootstrap][widget]") {
    Harness harness;
    LogCapture capture;
    fv::BootstrapHooks hooks = harness.hooks();
    hooks.credentialBackend = [] {
        return std::make_shared<SilentKeyring>();
    };
    const std::string warning = "root: " + std::string(fv::kUnresponsiveKeyringStatus);
    std::shared_ptr<CapturedLog> log = capture.log;
    bool shown = false;
    hooks.run = [log, warning, &shown](fv::Application&, fv::MainWindow& window) {
        shown = window.isVisible();
        fvtest::waitUntil([&] { return log->has(warning); }, 5000ms);
        return 123;
    };
    fv::Bootstrap bootstrap(std::move(hooks));
    CHECK(bootstrap.run(harness.argc, harness.argv) == 123);
    CHECK(shown);
    CHECK(capture.log->has(warning));
}

namespace {

// A feature that hears Bootstrap's lifecycle, as the controller does.
class LifecycleProbe final : public fv::Feature {
public:
    explicit LifecycleProbe(std::vector<std::string>& heard) : heard_(heard) {}
    std::string_view id() const override { return "probe"; }
    std::vector<std::string_view> required_features() const override { return {"shell"}; }
    void provide(fv::AppContext& context) override {
        auto lifecycle = std::make_shared<fv::ShellLifecycle>();
        lifecycle->trayCreated = [this](fv::SystemTray&) {
            heard_.push_back("tray");
        };
        lifecycle->mainWindowShown = [this] {
            heard_.push_back("shown");
        };
        lifecycle->ambientStatus = [this](const std::string& status) {
            heard_.push_back("ambient:" + status);
        };
        context.provide(lifecycle);
        // Quit's steps run before the features go, the database's after the controller's.
        context.shutdown().add(fv::ShutdownStep::TranscriptionBegin, {},
                               [this] { heard_.push_back("quit"); });
    }
    void install(fv::AppContext&) override {}

private:
    std::vector<std::string>& heard_;
};

} // namespace

// A15: the database is in the context before the features, but opens on first use (as
// services/database.py's lazy manager), after the window is painted.
TEST_CASE("the database is there before the features, opens on first use and closes when Quit "
          "ends",
          "[widget]") {
    Harness harness;
    std::vector<std::string> heard;
    fv::BootstrapHooks hooks = harness.hooks();
    hooks.features = [&heard] {
        std::vector<std::unique_ptr<fv::Feature>> features;
        features.push_back(std::make_unique<fv::ShellFeature>());
        features.push_back(std::make_unique<LifecycleProbe>(heard));
        return features;
    };
    int version = 0;
    bool openedBefore = true;
    hooks.run = [&version, &openedBefore](fv::Application&, fv::MainWindow& window) {
        fv::Database& database = window.context().db();
        openedBefore = database.is_open();
        // Opening blocks, so it is asked off the GUI thread.
        version = std::async(std::launch::async, [&database] {
                      database.ensure_open();
                      return database.schema_version();
                  }).get();
        return 7;
    };
    fv::Bootstrap bootstrap(std::move(hooks));

    CHECK(bootstrap.run(harness.argc, harness.argv) == 7);

    CHECK_FALSE(openedBefore);
    CHECK(version == 18);
    REQUIRE(harness.has("database"));
    CHECK(harness.index("database") < harness.index("features"));
    CHECK(std::filesystem::exists(paths().user_file(fv::identity().database_file)));
    // The tray once made, then the window shown, then Quit's steps.
    CHECK(heard == std::vector<std::string>{"tray", "shown", "quit"});
}

namespace {

// The in-app MCP listener as the automation feature provides it.
class McpProbe final : public fv::Feature {
public:
    explicit McpProbe(std::vector<std::string>& heard) : heard_(heard) {}
    std::string_view id() const override { return "mcp_probe"; }
    void provide(fv::AppContext& context) override {
        context.provide<fv::agentapi::McpServerControl>(std::make_shared<Control>(heard_));
    }
    void install(fv::AppContext&) override {}

private:
    class Control final : public fv::agentapi::McpServerControl {
    public:
        explicit Control(std::vector<std::string>& heard) : heard_(heard) {}
        fv::agentapi::ServerStatus status() override { return {}; }
        std::string token() override { return {}; }
        void start(int, bool, std::function<bool()>) override {}
        void stop(bool wait) override { heard_.push_back(wait ? "mcp stop, waiting" : "mcp stop"); }

    private:
        std::vector<std::string>& heard_;
    };
    std::vector<std::string>& heard_;
};

} // namespace

// bootstrap.py _shut_down stops the MCP runtime, waiting, before the splash and the controllers.
TEST_CASE("quit stops the MCP listener, waiting, before the controllers clean up", "[widget]") {
    Harness harness;
    std::vector<std::string> heard;
    fv::BootstrapHooks hooks = harness.hooks();
    hooks.features = [&heard] {
        std::vector<std::unique_ptr<fv::Feature>> features;
        features.push_back(std::make_unique<fv::ShellFeature>());
        features.push_back(std::make_unique<LifecycleProbe>(heard));
        features.push_back(std::make_unique<McpProbe>(heard));
        return features;
    };
    fv::Bootstrap bootstrap(std::move(hooks));
    CHECK(bootstrap.run(harness.argc, harness.argv) == 123);
    CHECK(heard == std::vector<std::string>{"tray", "shown", "mcp stop, waiting", "quit"});
}

TEST_CASE("update health waits for database and the running event loop", "[widget]") {
    const bool acknowledged = GENERATE(false, true);
    Harness harness;
    LogCapture capture;
    auto hooks = harness.hooks();
    std::atomic<int> attempts{0};
    hooks.acknowledgeUpdate = [&] {
        ++attempts;
        return acknowledged;
    };
    hooks.run = [&](fv::Application& app, fv::MainWindow& window) {
        REQUIRE_FALSE(window.isEnabled());
        REQUIRE(window.context().find<fv::UpdateHealthLaunch>());
        REQUIRE(attempts.load() == 0);
        QTimer check;
        QObject::connect(&check, &QTimer::timeout, &window, [&] {
            if (window.isEnabled())
                app.exit(0);
        });
        check.start(10);
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &window, [&] { app.exit(2); });
        watchdog.start(6000);
        return app.run(&window);
    };
    fv::Bootstrap bootstrap(std::move(hooks));
    CHECK(bootstrap.run(harness.argc, harness.argv) == (acknowledged ? 0 : 1));
    CHECK(attempts.load() == 1);
    // bootstrap.py _schedule_update_acknowledgement's line before exit(1).
    CHECK(capture.log->has("root: Could not acknowledge the update transaction") == !acknowledged);
}

// M10.12's forced failure for the owner's rollback demo: OPENWHISPER_CPP_TEST_FAIL_HEALTH=1 makes a
// health launch exit 1 without acknowledging, and a normal start ignores it.
TEST_CASE("OPENWHISPER_CPP_TEST_FAIL_HEALTH fails only a health launch", "[widget]") {
    const bool healthLaunch = GENERATE(false, true);
    const auto saved = fv::testkit::get_env("OPENWHISPER_CPP_TEST_FAIL_HEALTH");
    fv::testkit::set_env("OPENWHISPER_CPP_TEST_FAIL_HEALTH", "1");
    Harness harness;
    LogCapture capture;
    auto hooks = harness.hooks();
    std::atomic<int> attempts{0};
    if (healthLaunch) {
        hooks.acknowledgeUpdate = [&] {
            ++attempts;
            return true;
        };
        hooks.run = [](fv::Application& app, fv::MainWindow& window) {
            QTimer watchdog;
            watchdog.setSingleShot(true);
            QObject::connect(&watchdog, &QTimer::timeout, &window, [&] { app.exit(2); });
            watchdog.start(6000);
            return app.run(&window);
        };
    }
    fv::Bootstrap bootstrap(std::move(hooks));
    const int code = bootstrap.run(harness.argc, harness.argv);
    fv::testkit::set_env("OPENWHISPER_CPP_TEST_FAIL_HEALTH", saved);

    CHECK(code == (healthLaunch ? 1 : 123));
    CHECK(attempts.load() == 0);
    CHECK(capture.log->has("root: OPENWHISPER_CPP_TEST_FAIL_HEALTH=1: failing this update health "
                           "check as a test") == healthLaunch);
}

// The "database unavailable" case of
// tests/test_bootstrap.py::test_update_health_requires_database_and_matching_transaction: the
// window shows, but a health launch whose database will not open is not acknowledged; it exits 1
// after "Update readiness failed", and the helper rolls the update back.
TEST_CASE("update health is never acknowledged when the database cannot be opened", "[widget]") {
    Harness harness;
    LogCapture capture;
    std::filesystem::create_directories(paths().user_file(fv::identity().database_file) / "x");
    auto hooks = harness.hooks();
    std::atomic<int> attempts{0};
    hooks.acknowledgeUpdate = [&] {
        ++attempts;
        return true;
    };
    bool ran = false;
    hooks.run = [&](fv::Application& app, fv::MainWindow& window) {
        ran = true;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &window, [&] { app.exit(2); });
        watchdog.start(6000);
        return app.run(&window);
    };
    fv::Bootstrap bootstrap(std::move(hooks));

    CHECK(bootstrap.run(harness.argc, harness.argv) == 1);

    CHECK(attempts.load() == 0);
    CHECK(ran);
    CHECK(capture.log->has_prefix("root: Update readiness failed: "));
    CHECK_FALSE(capture.log->has("root: Could not acknowledge the update transaction"));
}

// A15, as OpenWhisper: a database that will not open no longer ends the start with an error box.
// The window shows and the app runs; each use of the database fails with its error.
TEST_CASE("a database that cannot be opened leaves the app running with failing saves",
          "[widget]") {
    Harness harness;
    LogCapture capture;
    // A folder where the database file goes.
    std::filesystem::create_directories(paths().user_file(fv::identity().database_file) / "x");
    auto hooks = harness.hooks();
    bool saveFailed = false;
    hooks.run = [&saveFailed](fv::Application&, fv::MainWindow& window) {
        fv::Database& database = window.context().db();
        saveFailed = std::async(std::launch::async, [&database] {
                         try {
                             database.write([](fv::sql::Connection& connection) {
                                 connection.exec("SELECT 1");
                             });
                         } catch (const fv::sql::Error&) {
                             return true;
                         }
                         return false;
                     }).get();
        return 5;
    };
    fv::Bootstrap bootstrap(std::move(hooks));

    CHECK(bootstrap.run(harness.argc, harness.argv) == 5);

    CHECK(harness.errors.empty());
    CHECK(harness.has("features"));
    CHECK(harness.has("main_window_shown"));
    CHECK(saveFailed);
    CHECK(harness.has("shutdown"));
    CHECK_FALSE(capture.log->has("root: Could not initialize the database"));
}

TEST_CASE("an unresponsive keyring reaches the lifecycle's ambient status", "[widget]") {
    Harness harness;
    std::vector<std::string> heard;
    fv::BootstrapHooks hooks = harness.hooks();
    hooks.features = [&heard] {
        std::vector<std::unique_ptr<fv::Feature>> features;
        features.push_back(std::make_unique<fv::ShellFeature>());
        features.push_back(std::make_unique<LifecycleProbe>(heard));
        return features;
    };
    hooks.credentialBackend = [] {
        return std::make_shared<SilentKeyring>();
    };
    const std::string ambient = "ambient:" + std::string(fv::kUnresponsiveKeyringStatus);
    hooks.run = [&heard, &ambient](fv::Application&, fv::MainWindow&) {
        fvtest::waitUntil([&] { return std::ranges::find(heard, ambient) != heard.end(); }, 5000ms);
        return 0;
    };
    fv::Bootstrap bootstrap(std::move(hooks));

    CHECK(bootstrap.run(harness.argc, harness.argv) == 0);
    CHECK(std::ranges::find(heard, ambient) != heard.end());
}

// bootstrap.py main's four update_status / update_progress pairs, each shown before the event
// loop turns.
TEST_CASE("the splash shows the four startup steps in order", "[test_bootstrap][widget]") {
    Harness harness;
    fv::BootstrapHooks hooks = harness.hooks();
    std::vector<std::pair<QString, QString>> shown;
    hooks.processEvents = [&shown] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* splash = qobject_cast<fv::LoadingScreen*>(widget)) {
                shown.emplace_back(splash->statusText(), splash->progressText());
            }
        }
        QApplication::processEvents();
    };
    fv::Bootstrap bootstrap(std::move(hooks));
    CHECK(bootstrap.run(harness.argc, harness.argv) == 123);
    // The database opens while the splash's loop turns: count each step once.
    shown.erase(std::unique(shown.begin(), shown.end()), shown.end());
    const std::vector<std::pair<QString, QString>> expected{
        {QStringLiteral("Initializing components..."), QStringLiteral("Preparing startup...")},
        {QStringLiteral("Loading application..."), QStringLiteral("Loading runtime components...")},
        {QStringLiteral("Creating interface..."), QStringLiteral("Setting up windows...")},
        {QStringLiteral("Initializing audio system..."), QStringLiteral("Starting services...")},
    };
    CHECK(shown == expected);
}

// services/credentials._load_dotenv_into_environ through process_env.load_for_app: a variable
// already set to the .env file's value came from the file (an editor that started the app loaded
// it), so programs started for the user do not get it; one set to anything else is the user's.
TEST_CASE("startup keeps .env values an editor also set away from programs it starts", "[widget]") {
    Harness harness;
    fv::MutableEnv& env = fv::system_mutable_env();
    env.set("FV_AUDIT_EDITOR_KEY", "from-file");
    env.set("FV_AUDIT_USERS_KEY", "mine");
    {
        std::ofstream out(paths().env_file(), std::ios::binary);
        out << "FV_AUDIT_EDITOR_KEY=from-file\nFV_AUDIT_USERS_KEY=file-value\n"
               "FV_AUDIT_FILE_ONLY=never-loaded\n";
    }
    fv::Bootstrap bootstrap(harness.hooks());
    CHECK(bootstrap.run(harness.argc, harness.argv) == 123);

    const fv::EnvMap children = fv::ProcessEnv::instance().external_env();
    fv::ProcessEnv::instance().forget();
    env.unset("FV_AUDIT_EDITOR_KEY");
    env.unset("FV_AUDIT_USERS_KEY");
    CHECK_FALSE(children.contains("FV_AUDIT_EDITOR_KEY"));
    REQUIRE(children.contains("FV_AUDIT_USERS_KEY"));
    CHECK(children.at("FV_AUDIT_USERS_KEY") == "mine");
    // The file is read where a key is needed, never copied into this process's environment.
    CHECK_FALSE(children.contains("FV_AUDIT_FILE_ONLY"));
    CHECK_FALSE(env.get("FV_AUDIT_FILE_ONLY").has_value());
}

// from tests/test_bootstrap.py::test_main_hands_off_to_the_running_ui
TEST_CASE("main hands off to the running ui", "[test_bootstrap][widget]") {
    Harness harness;
    fv::BootstrapHooks hooks = harness.hooks();
    hooks.acquireInstance = [](const fv::Paths&) {
        return std::unique_ptr<fv::SingleInstance>();
    };
    fv::Bootstrap bootstrap(std::move(hooks));
    CHECK(bootstrap.run(harness.argc, harness.argv) == 0);
    CHECK(harness.has("instance"));
    CHECK_FALSE(harness.has("qt_app"));
    CHECK_FALSE(harness.has("loading_screen_shown"));
    CHECK_FALSE(harness.has("shutdown"));
}

// from tests/test_bootstrap.py::test_show_request_from_another_thread_runs_on_the_qt_thread
TEST_CASE("show request from another thread runs on the qt thread", "[test_bootstrap][widget]") {
    QObject context;
    std::atomic<int> ran{0};
    bool onQtThread = false;
    const auto callback = fv::qtThreadCallback(&context, [&] {
        onQtThread = QThread::currentThread() == qApp->thread();
        ++ran;
    });
    // The instance's peer may call from where no Qt event loop runs.
    std::thread caller(callback);
    caller.join();
    CHECK(fvtest::waitUntil([&ran] { return ran.load() == 1; }, 2000ms));
    CHECK(onQtThread);
}

TEST_CASE("a show request from a later launch restores the running window", "[widget]") {
    Harness harness;
    fv::BootstrapHooks hooks = harness.hooks();
    bool restored = false;
    hooks.run = [&harness, &restored](fv::Application&, fv::MainWindow& window) {
        window.hide();
        fv::SingleInstanceOptions options;
        options.lockFolder = harness.root.path() / "run";
        // The later launch, as another process would run it beside this event loop.
        CHECK(fvtest::onOtherThread([&options] {
            return fv::SingleInstance::acquire(paths().data_root(), fv::identity(), options) ==
                   nullptr;
        }));
        restored = fvtest::waitUntil([&window] { return window.isVisible(); }, 3000ms);
        return 0;
    };
    fv::Bootstrap bootstrap(std::move(hooks));
    CHECK(bootstrap.run(harness.argc, harness.argv) == 0);
    CHECK(restored);
}

// from tests/test_backup_startup.py::test_normal_startup_holds_shared_lease_without_restore
TEST_CASE("normal startup holds shared lease without restore", "[test_backup_startup][platform]") {
    fv::testkit::TempDataRoot root;
    const fv::DataLease lease = fv::acquireStartupDataLease(paths());
    CHECK(lease.held());
    CHECK(lease.mode() == fv::LeaseMode::Shared);
    {
        const fv::DataLease reader =
            fv::DataLease::acquire(paths().data_root(), fv::identity(), fv::LeaseMode::Shared);
        CHECK(reader.held());
    }
    CHECK_THROWS_AS(
        fv::DataLease::acquire(paths().data_root(), fv::identity(), fv::LeaseMode::Exclusive),
        fv::DataLeaseBusy);
}

// from tests/test_backup_startup.py::test_main_gates_restore_after_worker_modes_before_ui_import
// (adapted: the order is observed as it runs, where Python read main.py's source)
TEST_CASE("main gates restore after worker modes before ui import",
          "[test_backup_startup][widget]") {
    Harness harness;
    CHECK(fv::appMain(harness.argc, harness.argv, harness.hooks()) == 123);
    REQUIRE(harness.has("lease"));
    CHECK(harness.index("early_cli") < harness.index("lease"));
    CHECK(harness.index("lease") < harness.index("qt_app"));
    // The lease is released when the app ends.
    CHECK_NOTHROW(
        fv::DataLease::acquire(paths().data_root(), fv::identity(), fv::LeaseMode::Exclusive));

    Harness early;
    fv::BootstrapHooks hooks = early.hooks();
    hooks.arguments = std::vector<std::string>{"openwhisper-cpp", "--version"};
    CHECK(fv::appMain(early.argc, early.argv, std::move(hooks)) == 0);
    CHECK(early.has("early_cli"));
    CHECK_FALSE(early.has("lease"));
    CHECK_FALSE(early.has("qt_app"));
}

TEST_CASE("a data folder another process restores refuses the start", "[widget]") {
    Harness harness;
    const fv::DataLease restoring =
        fv::DataLease::acquire(paths().data_root(), fv::identity(), fv::LeaseMode::Exclusive);
    CHECK(fv::appMain(harness.argc, harness.argv, harness.hooks()) == 1);
    CHECK_FALSE(harness.has("lease"));
    CHECK_FALSE(harness.has("qt_app"));
    REQUIRE(harness.errors.size() == 1);
    CHECK(
        harness.errors[0] ==
        "Could not safely open application data: Another OpenWhisper_CPP process is restoring this "
        "data. Wait for it to finish, then launch again. The backup restore remains pending. "
        "Close other OpenWhisper_CPP processes and start the app again.");
}

namespace {

// A data root with a database at the current schema and a settings file, backed up and staged as
// a restore, off the GUI thread (backups block on SQLite and the disk). Returns the bundle.
std::filesystem::path stageRestoreOf(const std::filesystem::path& root,
                                     const std::filesystem::path& bundles) {
    const std::filesystem::path archive = bundles / "OpenWhisper_CPP-backup.fvbackup";
    std::filesystem::create_directories(bundles);
    {
        std::ofstream out(root / fv::identity().settings_file, std::ios::binary | std::ios::trunc);
        out << "{\"selected_model\": \"parakeet\"}";
    }
    std::async(std::launch::async, [&] {
        fv::Database::open_or_create(root / fv::identity().database_file).close();
        fv::backup::CreateOptions options;
        options.data_root = root;
        options.app_version = "0.1.0";
        fv::backup::create_backup(archive, options);
        // Under the resolved root, as the coordinator stages it: a stage made under an 8.3 short
        // path (the temporary folder's) does not match the root the startup gate applies it to.
        fv::backup::prepare_restore(archive, std::filesystem::weakly_canonical(root));
    }).get();
    return archive;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

// A staged restore is applied by the startup lease (backup::acquire_startup_data_lease), before
// Bootstrap opens the settings or the database; what it replaced is kept in previous-*.
TEST_CASE("a pending restore is applied before the settings open",
          "[test_backup_startup][widget]") {
    Harness harness;
    const std::filesystem::path root = paths().data_root();
    stageRestoreOf(root, harness.root.path() / "bundles");
    REQUIRE(fv::backup::pending_restore_exists(root));
    const std::filesystem::path settingsFile = paths().user_file(fv::identity().settings_file);
    const std::string staged = readText(settingsFile);
    // The data changes after the backup: the restore puts the bundle's back.
    {
        std::ofstream out(settingsFile, std::ios::binary | std::ios::trunc);
        out << "{\"changed_after_backup\": true}";
    }

    bool pendingAtLease = true;
    std::string settingsAtLease;
    fv::BootstrapHooks hooks = harness.hooks();
    hooks.step = [&](std::string_view step) {
        harness.steps.emplace_back(step);
        if (step == "lease") {
            pendingAtLease = fv::backup::pending_restore_exists(root);
            settingsAtLease = readText(settingsFile);
        }
    };
    CHECK(fv::appMain(harness.argc, harness.argv, std::move(hooks)) == 123);

    CHECK(harness.index("lease") < harness.index("qt_app"));
    CHECK_FALSE(pendingAtLease);
    CHECK(settingsAtLease.find("changed_after_backup") == std::string::npos);
    const std::optional<std::filesystem::path> previous = fv::backup::previous_data_dir(root);
    REQUIRE(previous.has_value());
    CHECK(previous->filename().string().starts_with("previous-"));
    CHECK(readText(*previous / fv::identity().settings_file).find("changed_after_backup") !=
          std::string::npos);
    CHECK_FALSE(staged.empty());
}

TEST_CASE("a pending restore another process blocks refuses the start with the error box",
          "[test_backup_startup][widget]") {
    Harness harness;
    const std::filesystem::path root = paths().data_root();
    stageRestoreOf(root, harness.root.path() / "bundles");
    // Another OpenWhisper_CPP process still uses the data.
    const fv::DataLease other = fv::DataLease::acquire(root, fv::identity(), fv::LeaseMode::Shared);

    CHECK(fv::appMain(harness.argc, harness.argv, harness.hooks()) == 1);

    CHECK_FALSE(harness.has("lease"));
    CHECK_FALSE(harness.has("qt_app"));
    REQUIRE(harness.errors.size() == 1);
    CHECK(harness.errors[0] ==
          "Could not safely open application data: A backup restore is pending, but another "
          "OpenWhisper_CPP process is still using this data. Close every OpenWhisper_CPP window "
          "and History "
          "API process, then launch again to finish the restore. The backup restore remains "
          "pending. Close other OpenWhisper_CPP processes and start the app again.");
    CHECK(fv::backup::pending_restore_exists(root));
}
