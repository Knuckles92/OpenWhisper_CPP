// The component runtime: installs and removals of the speech runtimes and their messages. The
// component tests of OpenWhisper's tests/test_application_controller.py, over a fake engine, the
// fake HTTP client serving the runtime archive and manual executors. A runtime is never replaced
// in the running process (architecture 5.7), so OpenWhisper's activation stands for "no runtime
// is loaded yet".

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

#include "base/identity.h"
#include "model_runtime_harness.h"

using namespace fv::models;
using namespace fv::models::test;
using Catch::Matchers::ContainsSubstring;
using fv::Json;

namespace {

Json last_finish(const RuntimeHarness& h) {
    const Json finish = h.last_entry("component_install_finished");
    REQUIRE(finish.is_array());
    return finish;
}

std::string restart_gpu() {
    return "Restart " + std::string(fv::identity().product) + " to switch to the GPU runtime.";
}

std::string restart_enable() {
    return "Restart " + std::string(fv::identity().product) + " to enable this component.";
}

} // namespace

TEST_CASE("TestApplicationController::test_component_install_activates_and_reloads_without_"
          "restart",
          "[test_application_controller]") {
    // No runtime is loaded in this process yet, so the new one is used at once: the engine
    // reloads and no restart is asked for.
    RuntimeHarness h;
    h.build();

    h.on_ui([&] { h.components->request_install(kCpuRuntime); });
    h.settle(true);

    const Json finish = last_finish(h);
    CHECK(finish[1] == kCpuRuntime);
    CHECK(finish[2] == true);
    CHECK(finish[3] == "Installed and ready to use — no restart needed.");
    CHECK_FALSE(finish[3].get<std::string>().find("Restart") != std::string::npos);
    CHECK(h.store->is_installed(kCpuRuntime));
    CHECK(h.engine.reloads() == 1);
    // The engines on that runtime were closed before the install.
    CHECK(h.engine.released() == std::vector<std::string>{kCpuRuntime});
}

TEST_CASE("TestApplicationController::test_component_activation_failure_keeps_the_restart_"
          "message",
          "[test_application_controller]") {
    SECTION("the GPU runtime beside a loaded CPU runtime waits for the next start") {
        RuntimeHarness h;
        h.build();
        h.engine.set_loaded_runtime_dir(h.temp.path() / "components" / kCpuRuntime);

        h.on_ui([&] { h.components->request_install(kCudaRuntime); });
        h.settle(true);

        const Json finish = last_finish(h);
        CHECK(finish[2] == true);
        CHECK(finish[3] == restart_gpu());
        CHECK_THAT(finish[3].get<std::string>(), ContainsSubstring("Restart"));
        CHECK(h.store->is_installed(kCudaRuntime));
        CHECK(h.engine.reloads() == 0);
    }
    SECTION("an update of the loaded runtime is staged for the next start") {
        RuntimeHarness h;
        h.build();
        make_installed(h.store->root(), kCpuRuntime, Json{{"version", "0.0.9"}});
        const auto release = h.leases.acquire(kCpuRuntime);

        h.on_ui([&] { h.components->request_install(kCpuRuntime); });
        h.settle(true);

        const Json finish = last_finish(h);
        CHECK(finish[2] == true);
        CHECK(finish[3] == restart_enable());
        CHECK(fs::exists(h.store->pending_dir() / kCpuRuntime));
        // The install closed the engines on the loaded runtime; its folder is still in place, so
        // the engine loads again from it rather than staying closed until the restart.
        CHECK(h.engine.calls() ==
              std::vector<std::string>{"release_backends_using asr-nvidia-cpu", "reload_engine"});
        release();
    }
}

TEST_CASE("a failed or canceled runtime install reloads nothing", "[models]") {
    RuntimeHarness h;
    h.build();
    make_installed(h.store->root(), kCpuRuntime, Json{{"version", "0.0.9"}});
    const auto release = h.leases.acquire(kCpuRuntime);
    h.on_ui([&] {
        h.components->request_install(kCpuRuntime);
        h.components->cancel_install(kCpuRuntime);
    });
    h.settle(true);
    CHECK(last_finish(h)[2] == false);
    CHECK(h.engine.reloads() == 0);
    release();
}

TEST_CASE("TestApplicationController::test_runtime_removal_is_blocked_during_a_meeting",
          "[test_application_controller]") {
    RuntimeHarness h;
    h.build();
    make_installed(h.store->root(), kCpuRuntime, Json{{"version", "cpu"}});
    h.engine.set_meeting_active(true);

    h.on_ui([&] { h.components->request_uninstall(kCpuRuntime); });
    h.settle(true);

    CHECK(h.store->is_installed(kCpuRuntime));
    CHECK(h.component_strand.queued() == 0);
    CHECK(h.statuses ==
          std::vector<std::string>{"Finish the current speech job before removing its runtime"});
    CHECK(h.engine.released().empty());
}

TEST_CASE("a runtime install is refused during a speech job", "[models]") {
    RuntimeHarness h;
    h.build();
    h.engine.set_job_active(true);

    h.on_ui([&] { h.components->request_install(kCpuRuntime); });

    CHECK(h.statuses ==
          std::vector<std::string>{"Finish the current speech job before installing its runtime"});
    CHECK(h.count("component_install_started") == 0);
    CHECK(h.component_strand.queued() == 0);
    CHECK_FALSE(h.coordinator->is_installing(kCpuRuntime));
}

TEST_CASE("a second install of a component in flight is not started", "[models]") {
    RuntimeHarness h;
    h.build();
    h.on_ui([&] {
        h.components->request_install(kCpuRuntime);
        h.components->request_install(kCpuRuntime);
    });
    CHECK(h.count("component_install_started") == 1);
    CHECK(h.component_strand.queued() == 1);
    h.settle(true);
    CHECK_FALSE(h.coordinator->is_installing(kCpuRuntime));
}

TEST_CASE("a canceled install says so and leaves nothing installed", "[models]") {
    RuntimeHarness h;
    h.build();
    h.on_ui([&] {
        h.components->request_install(kCpuRuntime);
        h.components->cancel_install(kCpuRuntime);
    });
    h.settle(true);

    const Json finish = last_finish(h);
    CHECK(finish == Json::array({"component_install_finished", kCpuRuntime, false,
                                 "Installation canceled."}));
    CHECK_FALSE(h.store->is_installed(kCpuRuntime));
    CHECK(h.engine.reloads() == 0);
    CHECK(h.count("component_state_changed") == 1);
}

TEST_CASE("an install without a catalog entry asks to check the connection", "[models]") {
    RuntimeHarness h;
    h.catalog = Json{{"schema", 2}, {"components", Json::object()}};
    h.build();
    h.on_ui([&] { h.components->request_install(kCpuRuntime); });
    h.settle(true);

    CHECK(last_finish(h) ==
          Json::array({"component_install_finished", kCpuRuntime, false,
                       "Could not reach the download server. Check your internet connection and "
                       "try again."}));
    CHECK(h.client.requests().empty());
}

TEST_CASE("a failed install carries the store's message", "[models]") {
    RuntimeHarness h;
    h.archive = stored_zip({{"bin/ggml.dll", "ggml"}});
    h.catalog["components"][kCpuRuntime]["platforms"]["win_amd64"] = h.runtime_entry("cpu");
    h.build();
    h.on_ui([&] { h.components->request_install(kCpuRuntime); });
    h.settle(true);

    CHECK(last_finish(h) == Json::array({"component_install_finished", kCpuRuntime, false,
                                         "The speech runtime is missing required files."}));
}

TEST_CASE("an install's progress is throttled and ends with every phase", "[models]") {
    RuntimeHarness h;
    h.build();
    h.on_ui([&] { h.components->request_install(kCpuRuntime); });
    h.settle(true);
    // The clock never moves, so a report passes only when its whole percent changes or it is
    // final: each phase shows once, the extraction at 50 % and at its end.
    const auto size = static_cast<std::int64_t>(h.archive.size());
    CHECK(h.entries("component_progress") ==
          Json::array({Json::array({"component_progress", kCpuRuntime, "resolving", 0, 0}),
                       Json::array({"component_progress", kCpuRuntime, "downloading", size, size}),
                       Json::array({"component_progress", kCpuRuntime, "verifying", size, size}),
                       Json::array({"component_progress", kCpuRuntime, "extracting", 1, 2}),
                       Json::array({"component_progress", kCpuRuntime, "extracting", 2, 2}),
                       Json::array({"component_progress", kCpuRuntime, "finalizing", 0, 0})}));
    // The engines on the runtime close before the install starts.
    CHECK(h.trace.at(0) == Json::array({"release_backends_using asr-nvidia-cpu"}));
    CHECK(h.trace.at(1) == Json::array({"component_install_started", kCpuRuntime}));
    CHECK(h.last_entry("component_install_finished").at(2) == true);
}

TEST_CASE("a runtime removal closes its engines and reports", "[models]") {
    SECTION("removed now") {
        RuntimeHarness h;
        h.build();
        make_installed(h.store->root(), kCpuRuntime, Json{{"version", "cpu"}});
        h.on_ui([&] { h.components->request_uninstall(kCpuRuntime); });
        CHECK(h.component_strand.queued() == 1);
        h.settle(true);
        CHECK_FALSE(h.store->is_installed(kCpuRuntime));
        CHECK(h.statuses == std::vector<std::string>{"Component removed"});
        CHECK(h.engine.released() == std::vector<std::string>{kCpuRuntime});
        CHECK(h.count("component_state_changed") == 1);
    }
    SECTION("the loaded runtime goes at the next start") {
        RuntimeHarness h;
        h.build();
        make_installed(h.store->root(), kCpuRuntime, Json{{"version", "cpu"}});
        const auto release = h.leases.acquire(kCpuRuntime);
        h.on_ui([&] { h.components->request_uninstall(kCpuRuntime); });
        h.settle(true);
        CHECK(h.statuses ==
              std::vector<std::string>{"Restart " + std::string(fv::identity().product) +
                                       " to finish removing the component."});
        release();
    }
    SECTION("a job that started meanwhile stops the removal") {
        RuntimeHarness h;
        h.build();
        make_installed(h.store->root(), kCpuRuntime, Json{{"version", "cpu"}});
        h.on_ui([&] { h.components->request_uninstall(kCpuRuntime); });
        h.engine.set_job_active(true);
        h.settle(true);
        CHECK(h.store->is_installed(kCpuRuntime));
        CHECK(h.statuses == std::vector<std::string>{
                                "Could not remove the component: Finish the current speech job "
                                "before removing its runtime"});
        CHECK(h.count("component_state_changed") == 1);
    }
}

TEST_CASE("install finish messages", "[models]") {
    CHECK(install_finish_message(kCpuRuntime, true) ==
          "Installed and ready to use — no restart needed.");
    CHECK(install_finish_message(kCudaRuntime, true) ==
          "Installed and ready to use — no restart needed.");
    CHECK(install_finish_message(kCudaRuntime, false) == restart_gpu());
    CHECK(install_finish_message("asr-nvidia-vulkan", false) == restart_gpu());
    CHECK(install_finish_message(kCpuRuntime, false) == restart_enable());
    CHECK(is_runtime_component(kCpuRuntime));
    CHECK(is_runtime_component(kCudaRuntime));
    CHECK_FALSE(is_runtime_component("gpu-accel"));
}

TEST_CASE("a remote or cloud engine is not reloaded by a runtime install", "[models]") {
    for (const std::string backend : {"remote", "api"}) {
        RuntimeHarness h;
        h.build();
        h.engine.set_selected_backend(backend);
        h.on_ui([&] { h.components->request_install(kCpuRuntime); });
        h.settle(true);
        CHECK(last_finish(h)[2] == true);
        CHECK(h.engine.reloads() == 0);
    }
}
