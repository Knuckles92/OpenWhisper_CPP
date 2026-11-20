// Quit asks first while a component installs (ui_controller.py _confirm_quit_while_busy: its
// get_component_installing reads component_coordinator.is_any_installing()). The flag the
// question reads is set when an install starts and cleared when the last one finishes.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>

#include "base/identity.h"
#include "controller_support.h"
#include "http/testing/fake_http_client.h"
#include "models/catalog.h"
#include "models/component_runtime.h"
#include "ui_controller/quit_confirm.h"

namespace {

using namespace std::chrono_literals;
using fvtest::ControllerRig;
using fvtest::RigOptions;
using fvtest::waitUntil;

const std::string kCuda = "asr-nvidia-cuda";

} // namespace

TEST_CASE("quit asks first while a component installs", "[widget]") {
    // The download stays open until the test lets it finish.
    std::mutex mutex;
    std::condition_variable opened;
    bool release = false;
    std::atomic<bool> downloading{false};
    fv::http::FakeHttpClient http;
    http.set_handler([&](const fv::http::Request& request) {
        downloading = true;
        std::unique_lock lock(mutex);
        opened.wait(lock, [&release] { return release; });
        return fv::http::FakeHttpClient::serve(request, "not the archive");
    });
    const fv::Json entry{
        {"version", "cuda"},
        {"component_api", fv::models::kComponentApi},
        {"platform", "win_amd64"},
        {"install_bytes", 1000},
        {"archives", fv::Json::array({fv::Json{{"name", "runtime.zip"},
                                               {"url", "https://example.invalid/runtime.zip"},
                                               {"sha256", std::string(64, '0')},
                                               {"size_bytes", 15},
                                               {"extract", "zip"}}})}};
    const fv::Json catalog{{"schema", 2},
                           {"components", {{kCuda, {{"platforms", {{"win_amd64", entry}}}}}}}};
    RigOptions options;
    options.http = &http;
    options.modelServices = [&catalog](fv::ModelServiceOptions& services) {
        services.coordinator.catalog = &catalog;
        services.store.host->gpu = [] {
            fv::models::NvidiaGpu card;
            card.name = "NVIDIA GeForce RTX 3060";
            card.total_mib = 12288;
            card.compute_capability = std::pair{8, 6};
            return std::optional<fv::models::NvidiaGpu>(card);
        };
    };
    ControllerRig rig(options);
    REQUIRE(rig.startEngine());
    CHECK_FALSE(rig.activity().snapshot().component_installing);

    auto& components = rig.context().service<fv::models::ComponentRuntime>();
    components.request_install(kCuda);
    REQUIRE(waitUntil([&downloading] { return downloading.load(); }));
    CHECK(rig.activity().snapshot().component_installing);

    std::optional<std::string> asked;
    rig.ui().quitPrompt = [&asked](QWidget*, const fv::QuitQuestion& question) {
        asked = question.text.toStdString();
        return false; // No: the app stays open
    };
    rig.window().requestQuit();
    CHECK(asked == std::string(fv::identity().product) +
                       " is busy with a component install. Quitting now stops it before it "
                       "finishes.");

    {
        const std::lock_guard lock(mutex);
        release = true;
    }
    opened.notify_all();
    // The install ends (the archive does not verify), and with it the question.
    CHECK(waitUntil([&rig] { return !rig.activity().snapshot().component_installing; }));
    rig.plan().run();
}
