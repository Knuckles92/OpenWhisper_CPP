// Startup to the main window, and the idle footprint (architecture 12.3: a painted window in under
// 400 ms; idle RSS under 150 MB without a model). The real Bootstrap runs offscreen with the
// controller's feature graph over fakes (support/fake_platform.h): a fake keyboard hook, the
// fake audio system with one microphone, the fake speech engine, the in-memory credential store,
// a tray that never reaches the desktop, and this case's own temporary data folder.
//
// startup_to_window_s is Bootstrap's own metric, as OpenWhisper's is: entry into the GUI
// bootstrap through the main window appearing (OpenWhisper docs/release-health.md, "Startup
// measures entry into the GUI bootstrap through the main window appearing"); it never claims a
// model is ready. The QApplication already exists in a test process (fv_testkit_qt's main), so
// its construction is not in the number, and the features are the ones ControllerRig installs,
// not every feature the generated registry links into fiorivoce.exe. idle_rss_mb is the
// process's resident set after two seconds of idle event loop with the window shown, the fake
// engine loaded and no model.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QApplication>
#include <QElapsedTimer>

#include "base/metrics.h"
#include "credentials/testing/memory_backend.h"
#include "support/fake_platform.h"
#include "support/perf_support.h"
#include "ui_shell/application.h"
#include "ui_shell/bootstrap.h"
#include "ui_shell/single_instance.h"

namespace {

using namespace std::chrono_literals;

std::vector<std::unique_ptr<fv::Feature>> features(fvperf::FakePlatformState& state) {
    std::vector<std::unique_ptr<fv::Feature>> made;
    made.push_back(std::make_unique<fv::ShellFeature>());
    made.push_back(std::make_unique<fv::RecordFeature>());
    made.push_back(std::make_unique<fv::HistoryFeature>());
    made.push_back(std::make_unique<fv::OverlayFeature>());
    made.push_back(std::make_unique<fv::SetupFeature>());
    made.push_back(std::make_unique<fv::SettingsFeature>());
    made.push_back(std::make_unique<fv::DownloadsFeature>());
    made.push_back(std::make_unique<fv::PersonalizeFeature>());
    made.push_back(std::make_unique<fv::ControllerFeature>(fvperf::fakePlatform(state)));
    return made;
}

} // namespace

TEST_CASE("startup to the main window and idle footprint", "[perf]") {
    fv::testkit::TempDataRoot root;
    fvtest::ScopedPerUserFolder perUser(root.path());
    fvperf::FakePlatformState state;
    auto engine = std::make_shared<fv::speech::FakeEngine>(fv::speech::FakeScript{});

    std::optional<double> startupToWindow;
    fv::metrics::set_sink([&startupToWindow](std::string_view name, double value) {
        if (name == "startup_to_window_s" && !startupToWindow) {
            startupToWindow = value;
        }
    });

    fv::BootstrapHooks hooks;
    hooks.features = [&state] {
        return features(state);
    };
    hooks.setupLogging = [](const fv::Paths&) {
        return std::shared_ptr<void>();
    };
    const std::filesystem::path locks = root.path() / "run";
    hooks.acquireInstance = [locks](const fv::Paths& paths) {
        fv::SingleInstanceOptions options;
        options.lockFolder = locks;
        options.handoff = 200ms;
        return fv::SingleInstance::acquire(paths.data_root(), fv::identity(), options);
    };
    hooks.credentialBackend = [] {
        return std::make_shared<fv::MemoryCredentialBackend>();
    };
    hooks.tray.available = [] {
        return true;
    };
    hooks.tray.showIcon = [](QSystemTrayIcon&) {
    };
    hooks.processEvents = [] {
        QApplication::processEvents();
    };
    hooks.errorBox = [](const std::string& message) {
        FAIL_CHECK(message);
    };
    hooks.arguments = std::vector<std::string>{"fiorivoce"};
    hooks.audioBackend = [] {
        auto audio = std::make_shared<fv::audio::testing::FakeBackend>();
        audio->set_layout({{"MME"}}, {{"Microphone (Test)", 0, 1, 0, 44100.0}}, 0);
        return audio;
    };
    hooks.speechBackends = [engine] {
        return engine->factory();
    };
    hooks.httpClient = [] {
        // No proxy and nothing sent: no feature of this graph reaches the network at startup.
        fv::http::CurlClientOptions options;
        options.proxy = [](std::string_view) {
            return std::optional<std::string>();
        };
        return fv::http::make_curl_client(std::move(options));
    };

    std::optional<double> ownToWindowMs;
    std::optional<double> idleRss;
    std::optional<double> shownRss;
    bool exposed = false;
    const auto entered = fvperf::Clock::now();
    hooks.run = [&](fv::Application&, fv::MainWindow& window) {
        ownToWindowMs = fvperf::msSince(entered);
        shownRss = fvperf::currentRssMb();
        QElapsedTimer idle;
        idle.start();
        while (idle.elapsed() < 2000) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        exposed = window.isVisible();
        idleRss = fvperf::currentRssMb();
        window.hide();
        return 0;
    };

    int argc = 1;
    char program[] = "fiorivoce";
    char* argv[] = {program, nullptr};
    int result = -1;
    {
        fv::Bootstrap bootstrap(std::move(hooks));
        result = bootstrap.run(argc, argv);
    }
    fv::metrics::set_sink({});

    CHECK(result == 0);
    REQUIRE(startupToWindow.has_value());
    REQUIRE(idleRss.has_value());
    CHECK(exposed);

    const std::string test = "perf.startup to the main window and idle footprint";
    const fv::Json notes{
        {"measured_from", "Bootstrap::run entry (QApplication already made by the test main)"},
        {"test_clock_to_run_ms", ownToWindowMs.value_or(-1)},
        {"rss_at_window_mb", shownRss.value_or(-1)}};
    const auto startup =
        fvperf::record("startup_to_window_s", "s", {*startupToWindow}, test, notes);
    CHECK(fvperf::underCeiling(startup, startup.p50));
    const auto rss = fvperf::record("idle_rss_mb", "MB", {*idleRss}, test,
                                    fv::Json{{"idle_s", 2}, {"model", "none (fake engine)"}});
    CHECK(fvperf::underCeiling(rss, rss.p50));
}
