// Quit and the installed-agent scan: an io task scanning for agents waits in a probe (up to 6 s
// for --version, 10 s for the sign-in) that Quit used to sit out, since Quit waits for the io
// strand. Its CancelRoot step now stops the instance's lookups, which kills the running probe.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <QApplication>

#include "agents/installed_agents.h"
#include "appkit/app_context.h"
#include "audio/testing/fake_backend.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "credentials/testing/memory_backend.h"
#include "process/testing/fake_launcher.h"
#include "shell_fixture.h"
#include "testkit/testkit.h"
#include "ui_shell/application.h"
#include "ui_shell/bootstrap.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_shell/single_instance.h"
#include "ui_shell/system_tray.h"

using namespace std::chrono_literals;

namespace {

struct Witness {
    std::mutex mutex;
    std::vector<std::string> heard;
    std::chrono::steady_clock::time_point scanEnded;
    void hear(std::string what) {
        const std::lock_guard lock(mutex);
        heard.push_back(std::move(what));
        scanEnded = std::chrono::steady_clock::now();
    }
    std::vector<std::string> all() {
        const std::lock_guard lock(mutex);
        return heard;
    }
};

} // namespace

TEST_CASE("an agent scan waiting on a probe that never answers does not hold Quit", "[widget]") {
    fv::testkit::TempDataRoot root;
    auto witness = std::make_shared<Witness>();
    // A "claude" on PATH that the fake launcher runs until it is killed.
    const std::filesystem::path bin = root.path() / "bin";
    std::filesystem::create_directories(bin);
#ifdef _WIN32
    const std::filesystem::path program = bin / "claude.exe";
#else
    const std::filesystem::path program = bin / "claude";
#endif
    { std::ofstream(program, std::ios::binary) << ""; }
    std::filesystem::permissions(program, std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);
    auto launcher = std::make_shared<fv::FakeProcessLauncher>();
    for (int i = 0; i < 8; ++i) {
        fv::FakeProcessScript hangs;
        hangs.exit_code = std::nullopt;
        launcher->push(hangs);
    }
    const std::map<std::string, std::string> env{{"PATH", fv::path_to_utf8(bin)},
                                                 {"PATHEXT", ".EXE"}};
    auto agents = std::make_shared<fv::agents::InstalledAgents>(
        launcher, [env] { return env; }, root.path() / "home", root.path() / "temp");

    fv::BootstrapHooks hooks;
    hooks.features = [] {
        std::vector<std::unique_ptr<fv::Feature>> features;
        features.push_back(std::make_unique<fv::ShellFeature>());
        return features;
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
    hooks.arguments = std::vector<std::string>{"fiorivoce"};
    hooks.audioBackend = [] {
        return std::make_shared<fv::audio::testing::FakeBackend>();
    };
    std::chrono::steady_clock::time_point quitAt;
    hooks.run = [witness, agents, launcher, &quitAt](fv::Application&, fv::MainWindow& window) {
        fv::agents::set_installed_agents(agents);
        window.context().io().post([witness, agents] {
            const auto scan = agents->scan_agents(true);
            witness->hear("scan ended");
        });
        fvtest::waitUntil([&launcher] { return !launcher->records().empty(); }, 5000ms);
        quitAt = std::chrono::steady_clock::now();
        return 0;
    };
    int argc = 1;
    char programName[10] = "fiorivoce";
    char* argv[2] = {programName, nullptr};
    {
        fv::Bootstrap bootstrap(std::move(hooks));
        CHECK(bootstrap.run(argc, argv) == 0);
    }
    REQUIRE_FALSE(launcher->records().empty());
    CHECK(witness->all() == std::vector<std::string>{"scan ended"});
    CHECK(witness->scanEnded - quitAt < 2s);
    CHECK(launcher->records().front()->killed);
}
