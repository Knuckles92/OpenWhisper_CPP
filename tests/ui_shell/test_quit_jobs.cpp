// Quit and the jobs and io strands (A04-63): a job still running when Quit reaches the strands
// is waited for before the history drain, the database close and the context go, as
// OpenWhisper's executor.shutdown(wait=True, cancel_futures=True) waits for it; a task still
// running on the io strand is waited for, last, before the database closes and the context goes.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <QApplication>

#include "appcore/shutdown_plan.h"
#include "appkit/app_context.h"
#include "appkit/feature.h"
#include "audio/testing/fake_backend.h"
#include "base/identity.h"
#include "base/log.h"
#include "credentials/testing/memory_backend.h"
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

// Longer than the 5 s Quit gives the jobs strand before it says a job still runs.
constexpr auto kJobLength = std::chrono::milliseconds(6000);
// Longer than the 1 s it gives the io strand.
constexpr auto kIoTaskLength = std::chrono::milliseconds(2500);

class CapturedLog final : public fv::log::Sink {
public:
    void write(const fv::log::Record& record) override {
        const std::lock_guard lock(mutex);
        lines.push_back(std::string(record.logger) + ": " + std::string(record.message));
    }
    bool has(const std::string& line) {
        const std::lock_guard lock(mutex);
        return std::ranges::find(lines, line) != lines.end();
    }
    std::mutex mutex;
    std::vector<std::string> lines;
};

// What the job and Quit's later steps saw.
struct Witness {
    std::atomic<bool> running{false};
    std::atomic<bool> finished{false};
    std::mutex mutex;
    std::vector<std::string> heard;

    void hear(const std::string& what) {
        const std::lock_guard lock(mutex);
        heard.push_back(what + (running.load() ? " while the job ran" : " after the job"));
    }
    std::vector<std::string> all() {
        const std::lock_guard lock(mutex);
        return heard;
    }
};

// A service of the context, as the runtimes a job uses are: its release says when it came.
struct ContextService {
    explicit ContextService(std::shared_ptr<Witness> witness) : witness_(std::move(witness)) {}
    ~ContextService() { witness_->hear("context released"); }
    std::shared_ptr<Witness> witness_;
};

// A feature whose service lives in the context and which hears the history drain.
class JobProbe final : public fv::Feature {
public:
    explicit JobProbe(std::shared_ptr<Witness> witness) : witness_(std::move(witness)) {}
    std::string_view id() const override { return "job_probe"; }
    std::vector<std::string_view> required_features() const override { return {"shell"}; }
    void provide(fv::AppContext& context) override {
        context.provide(std::make_shared<ContextService>(witness_));
        context.shutdown().add(fv::ShutdownStep::Persistence, {},
                               [witness = witness_] { witness->hear("history drained"); });
        // After Bootstrap's own database step, which closes it.
        context.shutdown().add(fv::ShutdownStep::Database, {},
                               [witness = witness_] { witness->hear("database closed"); });
    }
    void install(fv::AppContext&) override {}

private:
    std::shared_ptr<Witness> witness_;
};

// What one quit saw: the probe's witness and the log.
struct QuitRun {
    std::shared_ptr<Witness> witness = std::make_shared<Witness>();
    std::shared_ptr<CapturedLog> log = std::make_shared<CapturedLog>();
};

// Starts the app, posts a task lasting `length` to the strand `strand` picks, and quits while it
// runs.
QuitRun quitDuring(const std::function<fv::Executor&(fv::AppContext&)>& strand,
                   std::chrono::milliseconds length) {
    fv::testkit::TempDataRoot root;
    QuitRun quit;
    fv::log::add_sink(quit.log);
    const std::shared_ptr<Witness> witness = quit.witness;

    fv::BootstrapHooks hooks;
    hooks.features = [witness] {
        std::vector<std::unique_ptr<fv::Feature>> features;
        features.push_back(std::make_unique<fv::ShellFeature>());
        features.push_back(std::make_unique<JobProbe>(witness));
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
    hooks.arguments = std::vector<std::string>{"openwhisper-cpp"};
    hooks.audioBackend = [] {
        return std::make_shared<fv::audio::testing::FakeBackend>();
    };
    hooks.run = [witness, &strand, length](fv::Application&, fv::MainWindow& window) {
        strand(window.context()).post([witness, length] {
            witness->running = true;
            std::this_thread::sleep_for(length);
            witness->running = false;
            witness->finished = true;
        });
        fvtest::waitUntil([&witness] { return witness->running.load(); }, 5000ms);
        return 0;
    };
    int argc = 1;
    char program[10] = "openwhisper-cpp";
    char* argv[2] = {program, nullptr};
    {
        fv::Bootstrap bootstrap(std::move(hooks));
        CHECK(bootstrap.run(argc, argv) == 0);
    }
    fv::log::remove_sink(quit.log);
    return quit;
}

} // namespace

TEST_CASE("quit waits for a running job before the database and the context go", "[widget]") {
    // A stop's save and decode, say, on the jobs strand.
    const QuitRun quit = quitDuring(
        [](fv::AppContext& context) -> fv::Executor& { return context.jobs(); }, kJobLength);
    CHECK(quit.witness->finished);
    CHECK(quit.witness->all() == std::vector<std::string>{"history drained after the job",
                                                          "database closed after the job",
                                                          "context released after the job"});
    CHECK(quit.log->has("root: A background job is still running; waiting for it to finish"));
}

TEST_CASE("quit waits for a running io task before the database and the context go", "[widget]") {
    // A page's read or a network request on the io strand. It is waited for last, after every
    // feature's step (the history drain among them) has had its chance to cancel it.
    const QuitRun quit = quitDuring(
        [](fv::AppContext& context) -> fv::Executor& { return context.io(); }, kIoTaskLength);
    CHECK(quit.witness->finished);
    CHECK(quit.witness->all() == std::vector<std::string>{"history drained while the job ran",
                                                          "database closed after the job",
                                                          "context released after the job"});
    CHECK(quit.log->has(
        "root: A background read or request is still running; waiting for it to finish"));
}
