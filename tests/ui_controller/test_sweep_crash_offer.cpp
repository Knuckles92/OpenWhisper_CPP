// The "speech engine crashed" CPU-runtime offer comes once per crash: the crash log is never
// cleared, so the crash already offered is remembered, whatever the answer. (OpenWhisper has no
// such offer: its faulthandler log is only written.)
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <fstream>
#include <optional>

#include "base/testing/manual_executor.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_controller/engine_setup.h"

namespace {

struct Start {
    int offers = 0;
    bool done = false;
};

// One start of the engine setup over a runtime folder holding the crashed module.
Start startOnce(fv::MemorySettingsStore& settings, const std::filesystem::path& runtime,
                const std::filesystem::path& offeredFile, const fv::log::CrashInfo& crash,
                bool accept) {
    Start result;
    fv::ManualExecutor io;
    fv::ManualExecutor ui;
    fv::EngineSetupPorts ports;
    ports.settings = &settings;
    ports.io = &io;
    ports.ui = &ui;
    ports.recover_components = [] {
    };
    ports.adopt_missing = [](const fv::Json&, const fv::CancelToken&) {
        return fv::models::AdoptionReport{};
    };
    ports.resolve = [runtime](const fv::Json&) {
        fv::models::ResolvedEngine engine;
        engine.runtime_dir = runtime;
        return engine;
    };
    ports.ensure_engine = [] {
        return true;
    };
    ports.last_crash = [crash] {
        return std::optional<fv::log::CrashInfo>(crash);
    };
    ports.offer_cpu_runtime = [&result, accept] {
        ++result.offers;
        return accept;
    };
    ports.crash_offered_file = offeredFile;
    fv::EngineSetup setup(std::move(ports));
    setup.start([&result] { result.done = true; });
    io.run_all();
    ui.run_all();
    io.run_all();
    return result;
}

} // namespace

TEST_CASE("a declined crash offer does not come back at the next start", "[unit]") {
    fv::testkit::TempDataRoot root;
    const auto runtime = root.path() / "asr-nvidia-cuda";
    std::filesystem::create_directories(runtime);
    std::ofstream(runtime / "ggml-cuda.dll") << "x";
    const auto offered = root.path() / "openwhisper-cpp.crash.log.offered";
    fv::MemorySettingsStore settings;
    const fv::log::CrashInfo crash{"2026-11-14T10:20:30Z", 0xC0000005u, "ggml-cuda.dll", ""};

    const Start first = startOnce(settings, runtime, offered, crash, false);
    CHECK(first.offers == 1);
    CHECK(first.done);
    CHECK(fv::crashAlreadyOffered(offered, crash));

    const Start second = startOnce(settings, runtime, offered, crash, false);
    CHECK(second.offers == 0);
    CHECK(second.done);

    // A later crash in the runtime is offered again.
    const fv::log::CrashInfo later{"2026-11-15T08:00:00Z", 0xC0000005u, "ggml-cuda.dll", ""};
    const Start third = startOnce(settings, runtime, offered, later, false);
    CHECK(third.offers == 1);
}

TEST_CASE("without a file to remember it in, the crash is offered at every start", "[unit]") {
    fv::testkit::TempDataRoot root;
    const auto runtime = root.path() / "asr-nvidia-cuda";
    std::filesystem::create_directories(runtime);
    std::ofstream(runtime / "ggml-cuda.dll") << "x";
    fv::MemorySettingsStore settings;
    const fv::log::CrashInfo crash{"2026-11-14T10:20:30Z", 0xC0000005u, "ggml-cuda.dll", ""};
    CHECK(startOnce(settings, runtime, {}, crash, false).offers == 1);
    CHECK(startOnce(settings, runtime, {}, crash, false).offers == 1);
}

TEST_CASE("crashAlreadyOffered matches the time, code and module", "[unit]") {
    fv::testkit::TempDataRoot root;
    const auto file = root.path() / "offered";
    const fv::log::CrashInfo crash{"2026-11-14T10:20:30Z", 0xC0000005u, "ggml-cuda.dll", "x.dmp"};
    CHECK_FALSE(fv::crashAlreadyOffered(file, crash));
    fv::rememberCrashOffered(file, crash);
    CHECK(fv::crashAlreadyOffered(file, crash));
    CHECK_FALSE(fv::crashAlreadyOffered(
        file, fv::log::CrashInfo{"2026-11-14T10:20:30Z", 0xC0000006u, "ggml-cuda.dll", ""}));
    CHECK_FALSE(fv::crashAlreadyOffered(
        file, fv::log::CrashInfo{"2026-11-14T10:20:30Z", 0xC0000005u, "nemo_speech.dll", ""}));
}
