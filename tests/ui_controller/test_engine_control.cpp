// models::EngineControl over the controller: what the model and component runtimes read of the
// dictation engine (the loaded model and runtime, a job, a meeting, the saved engine) and the
// release of the engine on a runtime that is installed or removed.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <future>
#include <string>

#include "controller_support.h"
#include "ui_controller/engine_control.h"

using fvtest::ControllerRig;
using fvtest::RigOptions;
using fvtest::waitUntil;

namespace {

fv::ControllerEngineControl engineControl(ControllerRig& rig) {
    fv::EngineControlPorts ports;
    ports.speech = &rig.speech();
    ports.app = &rig.app();
    ports.transcription = &rig.runtime();
    ports.activity = &rig.activity();
    ports.settings = &rig.settings();
    // Where the rig resolves the engine's runtime from.
    ports.components_root = rig.paths().data_root() / "components";
    return fv::ControllerEngineControl(std::move(ports));
}

} // namespace

TEST_CASE("the engine control reads the loaded model, its runtime and the saved engine",
          "[widget]") {
    ControllerRig rig;
    fv::ControllerEngineControl engine = engineControl(rig);
    CHECK(engine.loaded_model_key().empty());
    CHECK(engine.loaded_runtime_dir().empty());
    CHECK(engine.selected_backend() == "parakeet");

    REQUIRE(rig.startEngine());

    CHECK(engine.loaded_model_key() == "parakeet-v3");
    CHECK(engine.loaded_runtime_dir() == rig.paths().data_root() / "components" / "asr-nvidia-cpu");
    CHECK_FALSE(engine.job_active());
    CHECK_FALSE(engine.meeting_active());

    rig.activity().set_meeting_active(true);
    CHECK(engine.meeting_active());
    rig.activity().set_meeting_active(false);
}

TEST_CASE("a recording or a reload is a speech job", "[widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());
    fv::ControllerEngineControl engine = engineControl(rig);

    rig.engine().hold();
    engine.reload_engine();
    REQUIRE(waitUntil([&rig] { return rig.app().reload_machine().in_flight(); }));
    CHECK(engine.job_active());
    rig.engine().resume();
    REQUIRE(waitUntil([&rig] { return rig.app().reload_machine().settled(); }));
    CHECK_FALSE(engine.job_active());

    REQUIRE(rig.app().start_recording());
    REQUIRE(waitUntil([&rig] { return rig.runtime().is_recording(); }));
    CHECK(engine.job_active());
    rig.ui().cancelRecording();
}

TEST_CASE("installing the loaded runtime closes the engine on it, and only that one", "[widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());
    fv::ControllerEngineControl engine = engineControl(rig);

    engine.release_backends_using("asr-nvidia-cuda");
    QApplication::processEvents();
    CHECK(rig.speech().readiness(fv::speech::Slot::Dictation).state ==
          fv::speech::ReadyState::Ready);

    engine.release_backends_using("asr-nvidia-cpu");
    CHECK(waitUntil([&rig] {
        return rig.speech().readiness(fv::speech::Slot::Dictation).state ==
               fv::speech::ReadyState::Offline;
    }));
    CHECK(waitUntil([&engine] { return engine.loaded_model_key().empty(); }));
    // A runtime stays in the process: the folder it was loaded from is still the loaded one.
    CHECK(engine.loaded_runtime_dir() == rig.paths().data_root() / "components" / "asr-nvidia-cpu");
}

TEST_CASE("the API engine has no local model to protect", "[widget]") {
    RigOptions options;
    options.settings = fv::Json{{"selected_model", "api"}};
    ControllerRig rig(options);
    fv::ControllerEngineControl engine = engineControl(rig);
    rig.mainWindowShown();
    REQUIRE(waitUntil([&rig] {
        return rig.speech().readiness(fv::speech::Slot::Dictation).state ==
               fv::speech::ReadyState::Ready;
    }));

    CHECK(engine.selected_backend() == "api");
    CHECK(engine.loaded_model_key().empty());
    CHECK(engine.loaded_runtime_dir().empty());
}

TEST_CASE("model maintenance consults the speech slots even without a controller model",
          "[widget]") {
    ControllerRig rig;
    fv::ControllerEngineControl engine = engineControl(rig);
    fv::speech::BackendConfig config;
    config.backend = "parakeet";
    config.model_key = "meeting-model";
    config.model_path = std::filesystem::path("meeting.gguf");
    rig.speech().select(fv::speech::Slot::Meeting, std::move(config));
    REQUIRE(waitUntil([&rig] {
        return rig.speech().readiness(fv::speech::Slot::Meeting).state ==
               fv::speech::ReadyState::Ready;
    }));
    CHECK(engine.loaded_model_key().empty());

    const auto tryRemoval = [&](const char* key) {
        return std::async(std::launch::async,
                          [&engine, key] {
                              bool executed = false;
                              const bool allowed =
                                  engine.with_model_unused(key, [&] { executed = true; });
                              return std::pair(allowed, executed);
                          })
            .get();
    };
    const auto resident = tryRemoval("meeting-model");
    CHECK_FALSE(resident.first);
    CHECK_FALSE(resident.second);
    const auto unused = tryRemoval("different-model");
    CHECK(unused.first);
    CHECK(unused.second);

    rig.activity().set_meeting_active(true);
    const auto active = tryRemoval("still-unused-model");
    CHECK_FALSE(active.first);
    CHECK_FALSE(active.second);
}
