#include "ui_shell/bootstrap.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <QApplication>
#include <QMessageBox>
#include <QPointer>
#include <QTimer>

#include "agentapi/mcp_runtime.h"
#include "agents/agent_tasks.h"
#include "agents/installed_agents.h"
#include "appcore/activity.h"
#include "appcore/cancel_router.h"
#include "appcore/guided_setup_state.h"
#include "appcore/shutdown_plan.h"
#include "appkit/app_context.h"
#include "appkit/feature.h"
#include "appkit/qt_bridge.h"
#include "audio/backend.h"
#include "backup/startup_gate.h"
#include "base/clock.h"
#include "base/env.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/metrics.h"
#include "base/paths.h"
#include "credentials/dotenv.h"
#include "credentials/platform_backend.h"
#include "credentials/store.h"
#include "credentials/validate.h"
#include "db/database.h"
#include "engine/platform.h"
#include "http/abortable_client.h"
#include "http/client.h"
#include "log/setup.h"
#include "log/startup_profiler.h"
#include "net/reactor.h"
#include "process/launcher.h"
#include "process/process_env.h"
#include "settings/install.h"
#include "settings/store.h"
#include "speech/cloud_backend.h"
#include "speech/parakeet_backend.h"
#include "ui_shell/application.h"
#include "ui_shell/early_cli.h"
#include "ui_shell/loading_screen.h"
#include "ui_shell/main_window.h"
#include "ui_shell/qt_log.h"
#include "ui_shell/shell_feature.h"
#include "ui_shell/single_instance.h"
#include "ui_shell/system_tray.h"

namespace fv {
namespace {

// Python's logging.info and friends without a named logger.
constexpr log::Logger kRoot{"root"};

// Spelled out whole: packaging/windows/smoke-update.ps1 finds the switch by this name in the
// built executable before it relies on a forced rollback.
constexpr std::string_view kFailHealthVariable = "OPENWHISPER_CPP_TEST_FAIL_HEALTH";

const std::string kRule(60, '=');

// How long Quit waits for the io strand's running tasks (a page's read, a network request)
// before it logs that one is still running; it then keeps waiting, in slices this long.
constexpr std::chrono::milliseconds kIoShutdownBudget{1000};
// How long Quit waits for the jobs strand's running tasks (a recording's save, a decode wait)
// before it logs that one is still running; it then keeps waiting, in slices this long.
constexpr std::chrono::milliseconds kJobsShutdownBudget{5000};
// Accepted transcripts must survive Quit, so the history saves are drained, never dropped:
// Python's persistence_executor.shutdown(wait=True) waits as long as they take.
constexpr std::chrono::milliseconds kPersistenceShutdownBudget{60'000};
// The pool behind the strands (architecture 4: fv-pool-0..5).
constexpr int kPoolThreads = 6;

// The executable's name: the product in lower case.
std::string executableName() {
    std::string name(identity().product);
    std::ranges::transform(name, name.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return name;
}

// argparse's prog: the file name of the program as it was started.
std::string programName(const std::vector<std::string>& args) {
    if (args.empty() || args.front().empty()) {
        return executableName();
    }
    return path_to_utf8(path_from_utf8(args.front()).filename());
}

// _setup_transcription_backends' "api" entry: the hook's table answers first (a test's fakes),
// then the cloud backend over the network client, the credential store, the .env file and the
// upload strand Bootstrap owns. The speech service is built before any feature, so the entry
// cannot come from one.
speech::BackendFactory withApiEngine(speech::BackendFactory backends, http::Client* http,
                                     CredentialStore& credentials, const EnvReader& env,
                                     const DotEnv& dotenv, Executor& uploads) {
    return [backends = std::move(backends), http, &credentials, &env, &dotenv,
            &uploads](const speech::BackendConfig& config) -> std::unique_ptr<speech::Backend> {
        if (backends) {
            if (std::unique_ptr<speech::Backend> backend = backends(config)) {
                return backend;
            }
        }
        if (config.backend != "api" || http == nullptr) {
            return nullptr;
        }
        std::shared_ptr<const Json> settings = config.settings;
        if (settings == nullptr) {
            settings = std::make_shared<const Json>(Json::object());
        }
        return std::make_unique<speech::CloudSpeechBackend>(
            *http, credentials, env, dotenv, std::move(settings), uploads, system_clock());
    };
}

// Shuts `strand` down, waiting up to `budget`; true when its running tasks have all returned.
// Strand::shutdown waits on the condition "nothing runs" until a deadline, so it returns before
// the budget is spent only when that condition holds.
bool shutDownWithin(Strand& strand, std::chrono::milliseconds budget) {
    const auto started = std::chrono::steady_clock::now();
    strand.shutdown(budget);
    return std::chrono::steady_clock::now() - started < budget;
}

// Shuts `strand` down and waits for its running tasks however long they take: `budget`, then,
// after a warning that `what` still runs, further slices of the same length.
void waitForRunningTasks(Strand& strand, std::chrono::milliseconds budget, std::string_view what) {
    if (shutDownWithin(strand, budget)) {
        return;
    }
    kRoot.warning("{} is still running; waiting for it to finish", what);
    while (!shutDownWithin(strand, budget)) {
    }
}

} // namespace

// What one startup built, released by shutDown in this order's reverse.
struct Bootstrap::State {
    std::unique_ptr<SingleInstance> instance;
    std::unique_ptr<SettingsStore> settings;
    std::unique_ptr<CredentialStore> credentials;
    std::unique_ptr<Application> application;
    std::unique_ptr<qt::QtUiExecutor> ui;
    std::unique_ptr<ThreadPool> pool;
    std::unique_ptr<Strand> io;
    std::unique_ptr<Strand> jobs;
    std::unique_ptr<Strand> persistence;
    // The services M1 implements, built before the context and released after it.
    std::shared_ptr<audio::Backend> audio;
    std::shared_ptr<http::Client> http;
    // The same client: Quit aborts every transfer through it (ShutdownStep::CancelRoot).
    http::AbortableClient* httpAbort = nullptr;
    std::unique_ptr<net::Reactor> reactor;
    // The API engine's chunk uploads (a pool strand 3 wide) and the .env it reads keys from;
    // released after the speech service, whose backend uses them.
    std::unique_ptr<Strand> uploads;
    std::unique_ptr<DotEnv> dotenv;
    std::unique_ptr<speech::SpeechService> speech;
    std::unique_ptr<Database> database;
    std::unique_ptr<ActivityState> activity;
    std::unique_ptr<CancelRouter> cancel;
    std::unique_ptr<ShutdownPlan> shutdown;
    std::unique_ptr<AppContext> context;
    std::vector<std::unique_ptr<Feature>> features;
    std::unique_ptr<LoadingScreen> loadingScreen;
    // Provided by the shell feature; the context owns it.
    MainWindow* window = nullptr;
    std::unique_ptr<SystemTray> tray;
    std::optional<Paths> paths;
};

BootstrapHooks withDefaults(BootstrapHooks hooks) {
    if (hooks.env == nullptr) {
        hooks.env = &system_env();
    }
    if (!hooks.setupLogging) {
        const EnvReader* env = hooks.env;
        hooks.setupLogging = [env](const Paths& paths) -> std::shared_ptr<void> {
            auto handle = std::make_shared<log::LoggingHandle>(
                log::install_app_logging(paths, identity(), *env, hasConsoleOutput()));
            installQtMessageHandler();
            return handle;
        };
    }
    if (!hooks.acquireInstance) {
        hooks.acquireInstance = [](const Paths& paths) {
            return SingleInstance::acquire(paths.data_root(), identity());
        };
    }
    if (!hooks.credentialBackend) {
        hooks.credentialBackend = [] {
            return make_platform_backend();
        };
    }
    if (!hooks.processEvents) {
        hooks.processEvents = [] {
            QCoreApplication::processEvents();
        };
    }
    if (!hooks.run) {
        hooks.run = [](Application& application, MainWindow& window) {
            return application.run(&window);
        };
    }
    if (!hooks.acquireLease) {
        hooks.acquireLease = [](const Paths& paths) {
            return acquireStartupDataLease(paths);
        };
    }
    if (!hooks.audioBackend) {
        hooks.audioBackend = [] {
            return audio::make_portaudio_backend();
        };
    }
    if (!hooks.speechBackends) {
        hooks.speechBackends = [] {
            return speech::BackendFactory(
                [](const speech::BackendConfig& config) -> std::unique_ptr<speech::Backend> {
                    // Bootstrap::run adds the API engine over its own services; the remote
                    // engine arrives in M9, and the service reports "No speech engine is
                    // available" for it until then.
                    if (config.backend == "parakeet") {
                        return std::make_unique<speech::ParakeetBackend>(config);
                    }
                    return nullptr;
                });
        };
    }
    if (!hooks.httpClient) {
        hooks.httpClient = [] {
            return http::make_curl_client();
        };
    }
    if (!hooks.errorBox) {
        hooks.errorBox = [](const std::string& message) {
            // The stderr line stays when Qt itself cannot start.
            try {
                static int argc = 1;
                static std::string name = executableName();
                static char* argv[] = {name.data(), nullptr};
                std::unique_ptr<QApplication> owned;
                if (QApplication::instance() == nullptr) {
                    owned = std::make_unique<QApplication>(argc, argv);
                }
                QMessageBox::critical(nullptr, qt::toQString(identity().product),
                                      qt::toQString(message));
            } catch (...) {
            }
        };
    }
    return hooks;
}

Bootstrap::Bootstrap(BootstrapHooks hooks) : hooks_(withDefaults(std::move(hooks))) {
    if (!hooks_.features) {
        throw std::invalid_argument(
            "Bootstrap needs the feature list (FeatureRegistry::createAll).");
    }
}

Bootstrap::~Bootstrap() = default;

void Bootstrap::step(std::string_view name) const {
    if (hooks_.step) {
        hooks_.step(name);
    }
}

// ui_qt/bootstrap.py:250-397.
int Bootstrap::run(int& argc, char** argv) {
    log::StartupProfiler profiler;
    profiler.mark("main_entered");
    State state;
    state.paths.emplace(Paths::resolve(identity(), *hooks_.env));

    logging_ = hooks_.setupLogging(*state.paths);
    step("logging");
    profiler.mark("logging_ready");

    state.instance = hooks_.acquireInstance(*state.paths);
    step("instance");
    if (state.instance == nullptr) {
        return 0;
    }

    bool summaryLogged = false;
    int result = 0;
    std::exception_ptr failure;
    try {
        result = [&] {
            kRoot.info("{}", kRule);
            kRoot.info("Starting {}", identity().product);
            kRoot.info("{}", kRule);

            // Python imported its Qt classes here; a C++ binary has them linked, and the marks
            // stay so the profile reads as OpenWhisper's.
            profiler.mark("early_imports_started");
            profiler.mark("early_imports_finished");

            state.settings =
                std::make_unique<SettingsStore>(state.paths->user_file(identity().settings_file));
            state.application = std::make_unique<Application>(*state.settings, argc, argv);
            step("qt_app");
            profiler.mark("qt_app_created");
            // The first launch answers later launches from here on; until the server listens
            // they retry for two seconds.
            state.instance->listen();

            state.loadingScreen = std::make_unique<LoadingScreen>();
            profiler.mark("loading_screen_constructed");
            state.loadingScreen->show();
            step("loading_screen_shown");
            profiler.mark("loading_screen_shown");

            const auto showStep = [&](const StartupStep& startup) {
                state.loadingScreen->updateStatus(QString::fromUtf8(startup.status));
                state.loadingScreen->updateProgress(QString::fromUtf8(startup.progress));
            };
            showStep(kStartupSteps[0]);
            state.loadingScreen->repaint();
            step("process_events");
            hooks_.processEvents();
            profiler.mark("first_visual_flushed");

            showStep(kStartupSteps[1]);
            step("process_events");
            hooks_.processEvents();

            profiler.mark("late_imports_started");
            state.credentials =
                std::make_unique<CredentialStore>(hooks_.credentialBackend(), identity());
            state.pool = std::make_unique<ThreadPool>("fv-pool", kPoolThreads);
            state.io = std::make_unique<Strand>(*state.pool, "io", 4, ShutdownMode::CancelQueued);
            state.jobs =
                std::make_unique<Strand>(*state.pool, "jobs", 2, ShutdownMode::CancelQueued);
            state.persistence =
                std::make_unique<Strand>(*state.pool, "persistence", 1, ShutdownMode::Drain);
            state.ui = std::make_unique<qt::QtUiExecutor>(state.application.get());
            state.audio = hooks_.audioBackend();
            if (std::shared_ptr<http::Client> http = hooks_.httpClient()) {
                auto abortable = std::make_shared<http::AbortableClient>(std::move(http));
                state.httpAbort = abortable.get();
                state.http = std::move(abortable);
            }
            state.reactor = std::make_unique<net::Reactor>();
            auto launcher = make_process_launcher();
            agents::set_installed_agents(agents::make_installed_agents(launcher, *hooks_.env));
            agents::set_agent_task_services(*state.reactor, std::move(launcher));
            state.uploads =
                std::make_unique<Strand>(*state.pool, "uploads", speech::kChunkUploadConcurrency,
                                         ShutdownMode::CancelQueued);
            state.dotenv = std::make_unique<DotEnv>(state.paths->env_file(), *hooks_.env);
            // credentials._load_dotenv_into_environ without the copy: .env stays out of this
            // process's environment, but a variable an editor that started the app set to the
            // file's value is the app's, so programs started for the user do not inherit it.
            ProcessEnv::instance().adopt_dotenv(*state.dotenv);
            state.speech = std::make_unique<speech::SpeechService>(
                withApiEngine(hooks_.speechBackends(), state.http.get(), *state.credentials,
                              *hooks_.env, *state.dotenv, *state.uploads),
                *state.ui);
            state.activity = std::make_unique<ActivityState>();
            state.cancel = std::make_unique<CancelRouter>();
            state.shutdown = std::make_unique<ShutdownPlan>();
            profiler.mark("late_imports_finished");

            showStep(kStartupSteps[2]);
            step("process_events");
            hooks_.processEvents();

            // Before the database file exists: guided setup tells a fresh install from an
            // existing one by the data it finds (bootstrap.py does this ahead of the database too).
            appcore::guided_setup_state::initialize(*state.settings);

            // Before the window shows the engine an existing install uses.
            keep_unchosen_engine(*state.settings);

            // Not opened here (A15): services/database.py's lazy manager opens on first use,
            // after the window is painted (the controller's startup step asks first), and a
            // database that will not open leaves the app running with failing saves and the
            // error on the status line, not an error box that ends the start.
            state.database = std::make_unique<Database>(
                Database::deferred(state.paths->user_file(identity().database_file)));
            step("database");
            addShutdownSteps(state);

            AppServices services;
            services.identity = &identity();
            services.paths = &*state.paths;
            services.clock = &system_clock();
            services.settings = state.settings.get();
            services.credentials = state.credentials.get();
            services.db = state.database.get();
            services.http = state.http.get();
            services.net = state.reactor.get();
            services.audio = state.audio.get();
            services.speech = state.speech.get();
            services.ui = state.ui.get();
            services.jobs = state.jobs.get();
            services.persistence = state.persistence.get();
            services.io = state.io.get();
            services.shutdown = state.shutdown.get();
            services.cancel = state.cancel.get();
            services.activity = state.activity.get();
            state.context = std::make_unique<AppContext>(services);
            // A theme chosen in Settings goes through the application's own theme manager, the
            // one that follows the platform colour scheme. Not owned: the context goes first.
            state.context->provide(
                std::shared_ptr<Application>(state.application.get(), [](Application*) {}));
            if (hooks_.acknowledgeUpdate)
                state.context->provide(std::make_shared<UpdateHealthLaunch>());
            state.features = hooks_.features();
            FeatureRegistry::run(state.features, *state.context);
            step("features");

            // The shell feature built the window while the features installed, so they could plug
            // into it; their screens and menu items are all registered now.
            state.window = &state.context->service<MainWindow>();
            state.window->completeSetup();
            state.tray = std::make_unique<SystemTray>(*state.window, hooks_.tray);
            state.window->setTrayAvailable(state.tray->available());
            connectShell(*state.window, *state.tray);
            ShellLifecycle* lifecycle = state.context->find<ShellLifecycle>();
            if (lifecycle != nullptr && lifecycle->trayCreated) {
                lifecycle->trayCreated(*state.tray);
            }
            step("main_window_created");
            profiler.mark("ui_controller_created");

            showStep(kStartupSteps[3]);
            step("process_events");
            hooks_.processEvents();

            seed_new_install_settings(*state.settings);
            profiler.mark("application_controller_created");

            state.loadingScreen.reset();
            step("loading_screen_destroyed");

            if (hooks_.acknowledgeUpdate)
                state.window->setEnabled(false);
            state.window->restoreFromTray();
            MainWindow* window = state.window;
            state.instance->attachActivate(
                qtThreadCallback(window, [window] { window->restoreFromTray(); }));
            step("main_window_shown");
            profiler.mark("main_window_shown");

            // The store blocks on the OS, so it is asked off the GUI thread; the answer is
            // logged and shown on the status line when it comes.
            CredentialStore* credentials = state.credentials.get();
            Executor* ui = state.ui.get();
            std::function<void(const std::string&)> ambient =
                lifecycle != nullptr ? lifecycle->ambientStatus : nullptr;
            state.io->post([credentials, ui, ambient] {
                bool unresponsive = false;
                try {
                    unresponsive = keyring_did_not_respond(credentials->status());
                } catch (const std::exception& error) {
                    kRoot.error_with(error, "Could not ask the system keyring for its status");
                }
                if (unresponsive) {
                    ui->post([ambient] {
                        kRoot.warning("{}", kUnresponsiveKeyringStatus);
                        if (ambient) {
                            ambient(std::string(kUnresponsiveKeyringStatus));
                        }
                    });
                }
            });

            metrics::record("startup_to_window_s", profiler.events().back().second);

            // The engine load and the deferred startup work run once the window is visible,
            // never on the splash path.
            if (lifecycle != nullptr && lifecycle->mainWindowShown) {
                lifecycle->mainWindowShown();
            }

            if (hooks_.acknowledgeUpdate) {
                // The owner's rollback demo (M10.12): a build started with
                // OPENWHISPER_CPP_TEST_FAIL_HEALTH=1 fails its health launch, so the helper rolls
                // it back. Read here only: a normal start never looks at it.
                const bool failHealth = hooks_.env->get(kFailHealthVariable) == "1";
                // Wait for the real event loop and deferred feature setup before the helper can
                // retire its rollback tree; the database must open first.
                QTimer::singleShot(
                    1500, window,
                    [window, io = state.io.get(), ui, ack = hooks_.acknowledgeUpdate, failHealth,
                     database = state.database.get()] {
                        if (failHealth) {
                            kRoot.warning("{}=1: failing this update health check as a test",
                                          kFailHealthVariable);
                            QCoreApplication::exit(1);
                            return;
                        }
                        const auto complete =
                            [guard = QPointer<MainWindow>(window)](bool ready, bool notReady) {
                                if (!guard)
                                    return;
                                if (ready)
                                    guard->setEnabled(true);
                                else {
                                    // Python logs only the readiness error before exit(1) for that.
                                    if (!notReady)
                                        kRoot.error("Could not acknowledge the update transaction");
                                    QCoreApplication::exit(1);
                                }
                            };
                        io->post([ui, ack, complete, database] {
                            bool ok = false;
                            bool notReady = false;
                            try {
                                // _schedule_update_acknowledgement: a database that will not
                                // open is a readiness error; the update is not acknowledged.
                                database->ensure_open();
                            } catch (const std::exception& error) {
                                kRoot.error("Update readiness failed: {}", error.what());
                                notReady = true;
                            }
                            if (!notReady) {
                                try {
                                    ok = ack();
                                } catch (const std::exception& error) {
                                    kRoot.error_with(error, "Update readiness failed");
                                }
                            }
                            ui->post([complete, ok, notReady] { complete(ok, notReady); });
                        });
                    });
            }
            profiler.log_summary();
            summaryLogged = true;
            kRoot.info("Application initialization complete");
            kRoot.info("Starting event loop");
            step("run");
            return hooks_.run(*state.application, *state.window);
        }();
    } catch (const std::exception& error) {
        if (!summaryLogged) {
            profiler.log_summary();
        }
        kRoot.error_with(error, "Application startup failed");
        failure = std::current_exception();
    } catch (...) {
        if (!summaryLogged) {
            profiler.log_summary();
        }
        kRoot.error("Application startup failed");
        failure = std::current_exception();
    }

    // Before the rest of shutdown, so a new launch can take over while cleanup runs.
    state.instance->close();
    state.instance.reset();
    step("instance_released");
    shutDown(state);
    if (failure) {
        std::rethrow_exception(failure);
    }
    return result;
}

// ui_qt/bootstrap.py:433-454, with the steps M0 has; each failure is logged and the rest run.
void Bootstrap::shutDown(State& state) {
    // _shut_down's first step, mcp_runtime.stop(wait=True): no assistant's tool call reaches an
    // app whose services are closing.
    try {
        agentapi::McpServerControl* mcp =
            state.context != nullptr ? state.context->find<agentapi::McpServerControl>() : nullptr;
        if (mcp != nullptr) {
            mcp->stop(true);
        }
    } catch (const std::exception& error) {
        kRoot.error_with(error, "Failed to stop the MCP server");
    }

    try {
        if (state.loadingScreen != nullptr) {
            state.loadingScreen.reset();
        }
    } catch (const std::exception& error) {
        kRoot.error_with(error, "Failed to cleanup loading screen");
    }

    try {
        // Quit in OpenWhisper's order (A03-29): the features' steps, then the strands, the
        // history saves drained, the database closed.
        if (state.shutdown != nullptr) {
            state.shutdown->run();
        }
    } catch (const std::exception& error) {
        kRoot.error_with(error, "Failed to cleanup controllers");
    }

    try {
        if (state.io != nullptr) {
            // Done by Quit's database step already; again here for a start that failed before
            // the plan was made, so no io task outlives the context either way.
            waitForRunningTasks(*state.io, kIoShutdownBudget, "A background read or request");
        }
        if (state.tray != nullptr) {
            state.tray->hide();
        }
        state.tray.reset();
        // The context destroys the window it holds.
        state.window = nullptr;
        state.features.clear();
        state.context.reset();
        // The speech thread is joined; loaded runtimes stay loaded until the process ends.
        state.speech.reset();
        if (state.reactor) {
            agents::clear_agent_task_services();
            agents::set_installed_agents(nullptr);
            state.reactor.reset();
        }
        state.dotenv.reset();
        state.uploads.reset();
        state.database.reset();
        state.jobs.reset();
        state.persistence.reset();
        state.io.reset();
        state.pool.reset();
        state.shutdown.reset();
        state.cancel.reset();
        state.activity.reset();
        state.http.reset();
        state.audio.reset();
        state.ui.reset();
        state.credentials.reset();
    } catch (const std::exception& error) {
        kRoot.error_with(error, "Failed to cleanup controllers");
    }

    kRoot.info("{}", kRule);
    kRoot.info("Application shutdown complete");
    kRoot.info("{}", kRule);
    step("shutdown");
}

// The steps of Quit whose services Bootstrap owns; the features add theirs to the same plan.
void Bootstrap::addShutdownSteps(State& state) {
    // With the root cancel (OpenWhisper's _shutdown_cancel.set()): every HTTP transfer, running
    // or to come, ends within about a second, so no job or io task waits out a request's 120 s
    // idle timeout while Quit waits for it below. OpenWhisper leaves such requests to its daemon
    // threads.
    if (http::AbortableClient* http = state.httpAbort) {
        state.shutdown->add(ShutdownStep::CancelRoot, {}, [http] { http->abort_all(); });
    }
    // The installed-agent scan's probes (up to 6 s per candidate and 10 s for the sign-in, from
    // the io posts that scan) end with it too, and no new one starts: the io wait below would
    // otherwise sit them out. The instance in use is stopped, whoever set it.
    state.shutdown->add(ShutdownStep::CancelRoot, {}, [] {
        if (const std::shared_ptr<agents::InstalledAgents> agents =
                agents::installed_agents_if_set()) {
            agents->stop_lookups();
        }
    });
    Strand* jobs = state.jobs.get();
    Strand* persistence = state.persistence.get();
    Database* database = state.database.get();
    // executor.shutdown(wait=True, cancel_futures=True): the queued jobs are dropped and the
    // running ones waited for however long they take, because they use the database closed
    // below and the context destroyed after Quit (a stop's worker inside the live preview's
    // finalize, a save). The earlier steps make that wait short: they cancel the job slot, the
    // speech engine's work and the live preview, so a stop's worker returns without decoding.
    state.shutdown->add(ShutdownStep::Strands, "Error during executor shutdown", [jobs] {
        waitForRunningTasks(*jobs, kJobsShutdownBudget, "A background job");
    });
    state.shutdown->add(ShutdownStep::Persistence, {},
                        [persistence] { persistence->shutdown(kPersistenceShutdownBudget); });
    // The io strand (OpenWhisper's daemon threads: page reads, network requests, probes) goes
    // last, after every feature's step has cancelled what it owns, and is waited for before the
    // database closes: its tasks read the database and the context's services, and a C++ task
    // cannot be abandoned as a daemon thread is. Each blocks for a bounded time (a request's
    // connect and idle timeouts, under its caller's cancel token), so the wait ends.
    Strand* io = state.io.get();
    state.shutdown->add(ShutdownStep::Database, "Error during executor shutdown", [io] {
        waitForRunningTasks(*io, kIoShutdownBudget, "A background read or request");
    });
    state.shutdown->add(ShutdownStep::Database, "Error closing database",
                        [database] { database->close(); });
}

DataLease acquireStartupDataLease(const Paths& paths) {
    return backup::acquire_startup_data_lease(paths.data_root(), identity());
}

void showStartupError(const std::string& message,
                      const std::function<void(const std::string&)>& errorBox) {
    std::fprintf(stderr, "%s: %s\n", std::string(identity().product).c_str(), message.c_str());
    std::fflush(stderr);
    if (errorBox) {
        errorBox(message);
    }
}

std::function<void()> qtThreadCallback(QObject* context, std::function<void()> target) {
    QPointer<QObject> guarded(context);
    return [guarded, target = std::move(target)] {
        if (QObject* alive = guarded.data()) {
            qt::post(alive, target);
        }
    };
}

// main.py:159-416 for the desktop app.
int appMain(int argc, char** argv, BootstrapHooks hooks) {
    hooks = withDefaults(std::move(hooks));
    const auto step = [&hooks](std::string_view name) {
        if (hooks.step) {
            hooks.step(name);
        }
    };
    const std::vector<std::string> args =
        hooks.arguments ? *hooks.arguments : utf8_args(argc, argv);
    const EarlyCli cli = parseEarlyCli(args);
    step("early_cli");
    if (cli.command != EarlyCommand::Gui) {
        return runEarlyCommand(cli, programName(args), argc, argv);
    }

    // A restore must finish before the UI reads settings or opens the database; the shared
    // lease is then held for the process lifetime.
    const Paths paths = Paths::resolve(identity(), *hooks.env);
    DataLease lease;
    try {
        lease = hooks.acquireLease(paths);
    } catch (const std::exception& error) {
        showStartupError(std::format("Could not safely open application data: {} The backup "
                                     "restore remains pending. Close other {} processes and "
                                     "start the app again.",
                                     error.what(), identity().product),
                         hooks.errorBox);
        return 1;
    }
    step("lease");

    int code = 1;
    {
        Bootstrap bootstrap(std::move(hooks));
        try {
            code = bootstrap.run(argc, argv);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s: %s\n", std::string(identity().product).c_str(), error.what());
            code = 1;
        }
        // Bootstrap has closed every window and drained what it started; a restart must not
        // inherit the lease or race its own restore. Logging shuts down after it, as Python's
        // logging.shutdown().
        lease.release();
        backup::release_startup_data_lease();
    }
    return code;
}

} // namespace fv
