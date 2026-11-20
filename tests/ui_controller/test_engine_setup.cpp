// The speech engine's startup (architecture 5.7, 7.6): component recovery and adoption before
// any runtime loads, the setup dialog only when something is missing, the crash offer, and the
// loaded runtime's lease. Adoption runs against an OpenWhisper-style tree in the test's own
// temporary folder; nothing under the owner's OpenWhisper or FioriVoce folders is read or changed.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/log.h"
#include "controller_support.h"
#include "models/openwhisper_discovery.h"
#include "models/testing/fake_openwhisper.h"
#include "settings/keys.h"
#include "ui_controller/engine_setup.h"

using fvtest::ControllerRig;
using fvtest::RigOptions;
using fvtest::waitUntil;

namespace {

std::size_t loads(fv::speech::FakeEngine& engine) {
    const auto ops = engine.ops();
    return static_cast<std::size_t>(std::ranges::count(ops, std::string("load")));
}

// The records one logger wrote while the capture lived.
class LogCapture {
public:
    explicit LogCapture(std::string logger) : sink_(std::make_shared<Sink>(std::move(logger))) {
        fv::log::add_sink(sink_);
    }
    ~LogCapture() { fv::log::remove_sink(sink_); }
    bool contains(std::string_view text) const {
        std::lock_guard lock(sink_->mutex);
        return std::ranges::any_of(sink_->messages, [text](const std::string& message) {
            return message.find(text) != std::string::npos;
        });
    }

private:
    struct Sink final : fv::log::Sink {
        explicit Sink(std::string name) : logger(std::move(name)) {}
        void write(const fv::log::Record& record) override {
            if (record.logger == logger) {
                std::lock_guard lock(mutex);
                messages.emplace_back(record.message);
            }
        }
        std::string logger;
        std::mutex mutex;
        std::vector<std::string> messages;
    };
    std::shared_ptr<Sink> sink_;
};

bool isWithin(const std::filesystem::path& path, const std::filesystem::path& folder) {
    const auto relative = path.lexically_normal().lexically_relative(folder.lexically_normal());
    return !relative.empty() && *relative.begin() != "..";
}

} // namespace

// from
// tests/test_main_entrypoint.py::test_source_startup_activates_components_before_backend_imports
// (adapted: the component store's startup recovery runs, and its failure is logged, before the
// first runtime load; gpu-accel activation is not ported)
TEST_CASE("test_source_startup_activates_components_before_backend_imports",
          "[test_main_entrypoint][widget]") {
    LogCapture log("services.application_controller");
    RigOptions options;
    options.recoveryError = "the staging folder is locked";
    std::size_t loadsDuringRecovery = 99;
    ControllerRig* rigPointer = nullptr;
    options.duringRecovery = [&] {
        CHECK_FALSE(fv::is_gui_thread());
        loadsDuringRecovery = loads(rigPointer->engine());
    };
    ControllerRig rig(options);
    rigPointer = &rig;

    REQUIRE(rig.startEngine());

    CHECK(loadsDuringRecovery == 0);
    CHECK(loads(rig.engine()) == 1);
    const auto calls = rig.calls().all();
    REQUIRE(calls.size() >= 2);
    CHECK(calls[0] == "recover_components");
    CHECK(calls[1] == "adopt_missing");
    CHECK(log.contains("Component startup recovery failed"));
    // The loaded runtime's component stays leased.
    CHECK(rig.calls().has("acquire:asr-nvidia-cpu"));
}

TEST_CASE("an OpenWhisper install's runtime and model are adopted before the first load",
          "[widget]") {
    RigOptions options;
    std::filesystem::path components;
    std::filesystem::path models;
    const std::string weights = "parakeet weights";
    const fv::Json spec = fv::models::testing::small_model_spec("parakeet.gguf", weights);
    std::size_t loadsAtAdoption = 99;
    ControllerRig* rigPointer = nullptr;
    options.adoptMissing = [&](const fv::Json& settings, const fv::Paths& paths,
                               const fv::CancelToken& cancel) {
        loadsAtAdoption = loads(rigPointer->engine());
        // The fake install lives in the test's own data folder.
        const std::filesystem::path ow = paths.data_root() / "OpenWhisper";
        fv::models::testing::write_openwhisper_runtime(ow, "asr-nvidia-cpu", "win_amd64");
        fv::models::testing::write_openwhisper_model(ow, "parakeet-v3", spec,
                                                     {{"parakeet.gguf", weights}});
        fv::models::AdopterOptions adopter;
        adopter.artifacts = [&spec](std::string_view) {
            return spec;
        };
        fv::models::Host host;
        host.platform = "win_amd64";
        components = paths.data_root() / "components";
        models = paths.data_root() / "speech-models";
        fv::models::OpenWhisperAdopter adopt(components, models, {ow}, host, adopter);
        return adopt.adopt_missing(settings, cancel);
    };
    ControllerRig rig(options);
    rigPointer = &rig;

    REQUIRE(rig.startEngine());

    CHECK(loadsAtAdoption == 0);
    REQUIRE(isWithin(components, rig.paths().data_root()));
    CHECK(std::filesystem::exists(components / "asr-nvidia-cpu" / ".installed"));
    CHECK(std::filesystem::exists(models / "parakeet-v3" / "parakeet.gguf"));
    // Nothing was missing afterwards, so the setup dialog never opened.
    CHECK_FALSE(rig.calls().has("ensure_engine"));
}

TEST_CASE("the setup dialog opens only when the runtime or the model is missing", "[widget]") {
    {
        ControllerRig rig;
        REQUIRE(rig.startEngine());
        CHECK_FALSE(rig.calls().has("ensure_engine"));
    }
    {
        RigOptions options;
        options.engineInstalled = false;
        ControllerRig rig(options);
        rig.mainWindowShown();
        CHECK(waitUntil([&rig] { return rig.calls().has("ensure_engine"); }));
        // The dialog came before the load.
        CHECK(waitUntil([&rig] { return loads(rig.engine()) == 1; }));
    }
}

TEST_CASE("the setup dialog is not offered to an engine that needs no runtime", "[widget]") {
    RigOptions options;
    options.engineInstalled = false;
    options.settings = fv::Json{{"selected_model", "api"}};
    ControllerRig rig(options);
    rig.mainWindowShown();

    REQUIRE(waitUntil([&rig] { return rig.app().startup_database_ready(); }));
    CHECK_FALSE(rig.calls().has("ensure_engine"));
    // The API engine is put in the slot, which loads no model of its own.
    for (const fv::speech::FakeCall& call : rig.engine().calls()) {
        if (call.op == "load") {
            CHECK(call.model_key.empty());
        }
    }
    CHECK_FALSE(rig.app().reload_machine().in_flight());
}

TEST_CASE("a crash in the speech runtime offers the CPU runtime from the next start", "[widget]") {
    const bool accepted = GENERATE(false, true);
    RigOptions options;
    options.lastCrash =
        fv::log::CrashInfo{"2026-11-14T10:20:30Z", 0xC0000005u, "ggml-cuda.dll", ""};
    options.takeCpuRuntime = accepted;
    ControllerRig* rigPointer = nullptr;
    // The crashed module is one of the runtime folder's files.
    options.duringRecovery = [&rigPointer] {
        const auto runtime =
            rigPointer->paths().data_root() / "components" / "asr-nvidia-cpu" / "bin";
        std::filesystem::create_directories(runtime);
        std::ofstream(runtime / "GGML-CUDA.dll") << "ggml";
    };
    ControllerRig rig(options);
    rigPointer = &rig;

    REQUIRE(rig.startEngine());

    CHECK(rig.calls().has("offer_cpu_runtime"));
    CHECK(rig.settings().value("fv_speech_runtime", fv::Json("")) == (accepted ? "cpu" : ""));
}

TEST_CASE("a crash elsewhere offers nothing", "[widget]") {
    RigOptions options;
    options.lastCrash = fv::log::CrashInfo{"2026-11-14T10:20:30Z", 0xC0000005u, "Qt6Gui.dll", ""};
    ControllerRig rig(options);

    REQUIRE(rig.startEngine());

    CHECK_FALSE(rig.calls().has("offer_cpu_runtime"));
}

TEST_CASE("crashedInRuntime names only files of the runtime folder", "[unit]") {
    fv::testkit::TempDataRoot root;
    const auto runtime = root.path() / "asr-nvidia-cuda";
    std::filesystem::create_directories(runtime / "bin");
    std::ofstream(runtime / "bin" / "nemo_speech.dll") << "x";
    std::ofstream(runtime / "bin" / "ggml-base.dll") << "x";

    CHECK(fv::crashedInRuntime("NEMO_SPEECH.DLL", runtime));
    CHECK(fv::crashedInRuntime("ggml-base.dll", runtime));
    CHECK_FALSE(fv::crashedInRuntime("ggml-cpu.dll", runtime));
    CHECK_FALSE(fv::crashedInRuntime("unknown", runtime));
    CHECK_FALSE(fv::crashedInRuntime("nemo_speech.dll", root.path() / "missing"));
}

TEST_CASE("chooseCpuRuntime saves the CPU build for the next start", "[unit]") {
    fv::MemorySettingsStore settings;
    fv::chooseCpuRuntime(settings);
    CHECK(settings.get(fv::keys::fv_speech_runtime) == "cpu");
}
