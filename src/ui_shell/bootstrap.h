#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/data_lease.h"
#include "speech/service.h"
#include "ui_shell/system_tray.h"

class QObject;

namespace fv {

class Application;
class CredentialBackend;
class Database;
class EnvReader;
class Feature;
class MainWindow;
class Paths;
class SingleInstance;

namespace audio {
class Backend;
}
namespace http {
class Client;
}

// What Bootstrap tells the feature that drives the app (the controller, which provides it to
// the context) as startup proceeds. Each empty function is skipped.
// Affinity: GUI thread.
// Present only on an authenticated launch by the native update helper.
struct UpdateHealthLaunch {};
struct ShellLifecycle {
    // The tray exists (it is made after every feature installed).
    std::function<void(SystemTray&)> trayCreated;
    // The main window is shown: the engine loads and the deferred startup work runs
    // (app_controller.notify_main_ui_ready).
    std::function<void()> mainWindowShown;
    // Background news for the status line that must not replace a message (set_ambient_status):
    // the keyring that did not answer.
    std::function<void(const std::string& status)> ambientStatus;
    // Offers a restart when the installed executable changed and a feature reported a failure.
    std::function<bool(const QString& failure)> offerRestartForFailure;
};

// The status and progress lines the splash shows while the app starts (A01-33), in order.
struct StartupStep {
    const char* status;
    const char* progress;
};
inline constexpr StartupStep kStartupSteps[] = {
    {"Initializing components...", "Preparing startup..."},
    {"Loading application...", "Loading runtime components..."},
    {"Creating interface...", "Setting up windows..."},
    {"Initializing audio system...", "Starting services..."},
};

// What Bootstrap and appMain do that a test replaces. Each empty function takes its default.
// Python's tests patch the same seams (setup_logging, acquire_ui_instance, the runtime
// components, the event loop).
struct BootstrapHooks {
    // Every feature, constructed; FeatureRegistry::createAll in the executable. Required: the
    // generated registry is linked only into the executable.
    std::function<std::vector<std::unique_ptr<Feature>>()> features;
    // The process environment (system_env()).
    const EnvReader* env = nullptr;
    // The command line as UTF-8, the program first (utf8_args, which reads the wide command
    // line on Windows); appMain dispatches on it.
    std::optional<std::vector<std::string>> arguments;
    // Revalidates the transaction and records health after deferred startup.
    std::function<bool()> acknowledgeUpdate;
    // bootstrap.setup_logging: install_app_logging, then the Qt message handler. Returns what
    // must stay alive until exit.
    std::function<std::shared_ptr<void>(const Paths&)> setupLogging;
    // services/ui_instance.acquire_ui_instance for the data root; null hands off and quits.
    std::function<std::unique_ptr<SingleInstance>(const Paths&)> acquireInstance;
    // The OS credential store's backend (make_platform_backend).
    std::function<std::shared_ptr<CredentialBackend>()> credentialBackend;
    // How the tray reaches the desktop.
    TrayEnvironment tray;
    // QCoreApplication::processEvents(), between the splash's steps.
    std::function<void()> processEvents;
    // The event loop: shows the window and runs until quit (Application::run).
    std::function<int(Application&, MainWindow&)> run;
    // The startup lease of the data root (acquireStartupDataLease).
    std::function<DataLease(const Paths&)> acquireLease;
    // _show_startup_error's message box (a critical QMessageBox titled with the product).
    std::function<void(const std::string& message)> errorBox;
    // Each step as it happens ("early_cli", "lease", "logging", "instance", "qt_app",
    // "loading_screen_shown", "process_events", "database", "features", "main_window_created",
    // "loading_screen_destroyed", "main_window_shown", "run", "instance_released",
    // "shutdown"), for tests of the startup order.
    std::function<void(std::string_view step)> step;
    // The audio system (audio::make_portaudio_backend()).
    std::function<std::shared_ptr<audio::Backend>()> audioBackend;
    // The speech engines by selected_model key: Parakeet in process (speech::ParakeetBackend).
    std::function<speech::BackendFactory()> speechBackends;
    // The HTTP client (http::make_curl_client()).
    std::function<std::shared_ptr<http::Client>()> httpClient;
};

// ui_qt/bootstrap.py main: the startup sequence of the desktop app, run after the early command
// line and the data lease. Logging; the single-instance check (a hand-off returns 0 before any
// Qt object exists); the Qt application and the splash with its four status pairs; the
// settings migrations; the features, the main window and the tray; the splash goes and the
// window shows; the keyring check, the startup_to_window_s metric and the profiler summary;
// then the event loop. Whatever happens, the instance is released first, so a new launch can
// take over during cleanup, and every cleanup step runs, a failing one logged.
// Affinity: GUI thread, once per process.
class Bootstrap {
public:
    explicit Bootstrap(BootstrapHooks hooks);
    ~Bootstrap(); // shuts logging down last (logging.shutdown)
    Bootstrap(const Bootstrap&) = delete;
    Bootstrap& operator=(const Bootstrap&) = delete;

    // The event loop's exit code, or 0 after a hand-off. Rethrows a startup failure after
    // logging "Application startup failed".
    int run(int& argc, char** argv);

private:
    struct State;
    int start(State& state, int& argc, char** argv);
    void shutDown(State& state);
    void addShutdownSteps(State& state);
    void step(std::string_view name) const;

    BootstrapHooks hooks_;
    std::shared_ptr<void> logging_;
};

// main.py from _handle_early_cli to the end: the early command line (no Qt application, no
// lease, no runtime), then the startup data lease, which applies a pending restore first (refused
// with _show_startup_error's message and exit 1), then Bootstrap, then the lease released
// (release_startup_data_lease). The caller restarts the app on backup::restart_exit_code.
int appMain(int argc, char** argv, BootstrapHooks hooks);

// backup_startup.acquire_startup_data_lease for the data root (backup::acquire_startup_data_lease):
// a pending restore is applied under the exclusive lease before settings or SQLite open, then the
// shared lease is kept for the process lifetime. Throws StartupRestoreError with the sentence for
// each refusal, and the restore's own errors.
DataLease acquireStartupDataLease(const Paths& paths);

// main._show_startup_error: "<product>: <message>" on stderr, then the box.
void showStartupError(const std::string& message,
                      const std::function<void(const std::string&)>& errorBox);

// bootstrap.qt_thread_callback: a function any thread may call that runs target on context's
// thread (dropped once context is gone). A later launch's show request may arrive where no Qt
// event loop runs.
std::function<void()> qtThreadCallback(QObject* context, std::function<void()> target);

// The default hooks: the real logging, instance, credential backend, tray, event loop, lease
// and message box.
BootstrapHooks withDefaults(BootstrapHooks hooks);

} // namespace fv
