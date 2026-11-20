// Quit's streaming step (application_controller.py cleanup: _attempt(streaming_runtime.cleanup,
// "Error during streaming cleanup"), A04-63): the live preview is removed before the strands are
// waited for, so a stop's worker still finalizing it returns at once.
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "appcore/shutdown_plan.h"
#include "controller_support.h"
#include "dictation/streaming_runtime.h"

using fvtest::ControllerRig;
using fvtest::RigOptions;
using fvtest::waitUntil;

TEST_CASE("quit removes the live preview before it waits for the strands", "[widget]") {
    RigOptions options;
    options.settings["streaming_enabled"] = true;
    ControllerRig rig(options);
    auto& streaming = rig.context().service<fv::dictation::StreamingRuntime>();
    REQUIRE(rig.startEngine());
    REQUIRE(waitUntil([&streaming] { return streaming.configured(); }));

    std::vector<bool> configured;
    rig.plan().add(fv::ShutdownStep::Recorder, {},
                   [&] { configured.push_back(streaming.configured()); });
    rig.plan().add(fv::ShutdownStep::Strands, {},
                   [&] { configured.push_back(streaming.configured()); });
    rig.plan().run();

    // Still set up after the recorder's step, gone by the strands'.
    CHECK(configured == std::vector<bool>{true, false});
}
