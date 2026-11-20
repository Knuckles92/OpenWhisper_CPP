// Quit and a request that never ends (B33): Quit still waits for the io task, but aborts every
// HTTP transfer first (with the root cancel), so a request with a 120 s idle timeout on a
// silent server holds Quit for about a second, not two minutes.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
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
#include "credentials/testing/memory_backend.h"
#include "http/client.h"
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

// A server that took the request and never answers: send() returns only when its token is
// cancelled (checked as often as curl's transfer callback runs) or its idle timeout passes.
class SilentServerClient final : public fv::http::Client {
public:
    std::expected<fv::http::Response, fv::http::Error> send(const fv::http::Request& request,
                                                            const fv::CancelToken& cancel,
                                                            fv::http::BodySink*) override {
        entered = true;
        const auto deadline = std::chrono::steady_clock::now() + request.idle_timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancel.wait_for(100ms)) {
                return std::unexpected(fv::http::Error{fv::http::ErrorKind::Cancelled, "canceled"});
            }
        }
        return std::unexpected(fv::http::Error{fv::http::ErrorKind::Timeout, "timed out"});
    }
    std::atomic<bool> entered{false};
};

struct Witness {
    std::mutex mutex;
    std::vector<std::string> heard;
    void hear(std::string what) {
        const std::lock_guard lock(mutex);
        heard.push_back(std::move(what));
    }
    std::vector<std::string> all() {
        const std::lock_guard lock(mutex);
        return heard;
    }
};

class Probe final : public fv::Feature {
public:
    explicit Probe(std::shared_ptr<Witness> witness) : witness_(std::move(witness)) {}
    std::string_view id() const override { return "http_probe"; }
    std::vector<std::string_view> required_features() const override { return {"shell"}; }
    void provide(fv::AppContext& context) override {
        context.shutdown().add(fv::ShutdownStep::Database, {},
                               [witness = witness_] { witness->hear("database closed"); });
    }
    void install(fv::AppContext&) override {}

private:
    std::shared_ptr<Witness> witness_;
};

} // namespace

TEST_CASE("a request to a server that never answers does not hold Quit", "[widget]") {
    fv::testkit::TempDataRoot root;
    auto witness = std::make_shared<Witness>();
    auto silent = std::make_shared<SilentServerClient>();

    fv::BootstrapHooks hooks;
    hooks.features = [witness] {
        std::vector<std::unique_ptr<fv::Feature>> features;
        features.push_back(std::make_unique<fv::ShellFeature>());
        features.push_back(std::make_unique<Probe>(witness));
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
    hooks.httpClient = [silent]() -> std::shared_ptr<fv::http::Client> {
        return silent;
    };
    std::chrono::steady_clock::time_point quitAt;
    hooks.run = [witness, silent, &quitAt](fv::Application&, fv::MainWindow& window) {
        fv::http::Client* http = &window.context().http();
        // An io task with no cancel of its own, as a page's request is.
        window.context().io().post([witness, http] {
            fv::http::Request request;
            request.url = "http://127.0.0.1:9/never";
            request.idle_timeout = 120s;
            const auto answer = http->send(request, {});
            witness->hear(!answer && answer.error().kind == fv::http::ErrorKind::Cancelled
                              ? "request cancelled"
                              : "request ended otherwise");
        });
        fvtest::waitUntil([&silent] { return silent->entered.load(); }, 5000ms);
        quitAt = std::chrono::steady_clock::now();
        return 0;
    };
    int argc = 1;
    char program[10] = "openwhisper-cpp";
    char* argv[2] = {program, nullptr};
    {
        fv::Bootstrap bootstrap(std::move(hooks));
        CHECK(bootstrap.run(argc, argv) == 0);
    }
    REQUIRE(silent->entered);
    CHECK(std::chrono::steady_clock::now() - quitAt < 3s);
    CHECK(witness->all() == std::vector<std::string>{"request cancelled", "database closed"});
}
