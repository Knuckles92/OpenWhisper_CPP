// Startup recovery reaches the walkthrough captures (services/application_controller.py's
// startup recovery, recover_interrupted_recordings with the captures root): the composition
// gives the runtime walkthrough::captures_root, so expired walkthroughs are pruned at start.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "controller_support.h"
#include "walkthrough/library.h"
#include "walkthrough/storage.h"

TEST_CASE("startup recovery prunes expired walkthroughs under the captures root", "[widget]") {
    std::filesystem::path finished;
    fvtest::RigOptions options;
    options.beforeFeatures = [&finished](fv::AppContext& context) {
        const auto root = fv::walkthrough::captures_root(context.paths());
        finished = root / "2026-10-20_09-00-00";
        std::filesystem::create_directories(finished);
        std::ofstream(finished / "walkthrough.json") << "{}";
        const auto old = std::chrono::system_clock::now() - std::chrono::hours(24 * 10);
        std::filesystem::last_write_time(finished / "walkthrough.json",
                                         std::chrono::clock_cast<std::chrono::file_clock>(old));
        fv::walkthrough::set_retention(root, 1);
    };
    fvtest::ControllerRig rig(options);
    REQUIRE(std::filesystem::exists(finished));

    REQUIRE(rig.startEngine());

    CHECK(fvtest::waitUntil([&finished] { return !std::filesystem::exists(finished); }));
}
