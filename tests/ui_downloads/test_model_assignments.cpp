// What the Voice model page says about the engine, as functions over a settings snapshot, replayed
// against the vectors OpenWhisper's ModelAssignments methods give (tools/parity/cases/
// ui_qt.dialogs.settings_models.py) and checked where OpenWhisper_CPP differs.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/utf8.h"
#include "engine/runtime.h"
#include "golden/golden.h"
#include "ui_downloads/model_assignments.h"
#include "voice_model_support.h"

namespace {

using fv::Json;
using fv::golden::Case;

constexpr const char* kModule = "ui_qt.dialogs.settings_models";

// The vectors were taken before OpenAI's shutdown day (2027-02-26), when a saved retiring model
// still reads as itself; replayed on the system clock they would change on that day.
constexpr std::chrono::year_month_day kVectorDay{std::chrono::year{2026}, std::chrono::month{11},
                                                 std::chrono::day{15}};

} // namespace

TEST_CASE("ui_qt.dialogs.settings_models.voice_summary vectors", "[golden]") {
    fv::golden::for_each(kModule, "voice_summary", [](const Case& c) {
        CHECK(fv::voice_summary(c.arg<Json>(0), kVectorDay) == c.result<std::string>());
    });
}

TEST_CASE("a retired openai model reads as the default from the shutdown day", "[unit]") {
    const Json settings{{"selected_model", "api"}, {"api_transcription_model", "whisper-1"}};
    CHECK(fv::voice_summary(settings, kVectorDay) == "OpenAI · whisper-1");
    CHECK(fv::voice_summary(settings, std::chrono::year{2027} / std::chrono::month{2} /
                                          std::chrono::day{26}) == "OpenAI · gpt-transcribe");
}

TEST_CASE("ui_qt.dialogs.settings_models.voice_detail vectors", "[golden]") {
    fv::golden::for_each(kModule, "voice_detail", [](const Case& c) {
        CHECK(fv::voice_detail(c.arg<Json>(0)) == c.result<std::string>());
    });
}

TEST_CASE("ui_qt.dialogs.settings_models.voice_is_cloud vectors", "[golden]") {
    fv::golden::for_each(kModule, "voice_is_cloud", [](const Case& c) {
        CHECK(fv::voice_is_cloud(c.arg<Json>(0)) == c.result<bool>());
    });
}

TEST_CASE("ui_qt.dialogs.settings_models.voice_is_remote vectors", "[golden]") {
    fv::golden::for_each(kModule, "voice_is_remote", [](const Case& c) {
        CHECK(fv::voice_is_remote(c.arg<Json>(0)) == c.result<bool>());
    });
}

TEST_CASE("ui_qt.dialogs.settings_models.engine_caption vectors", "[golden]") {
    fv::golden::for_each(kModule, "engine_caption", [](const Case& c) {
        CHECK(fv::engine_caption(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("ui_qt.dialogs.settings_models.engine_filter vectors", "[golden]") {
    fv::golden::for_each(kModule, "engine_filter", [](const Case& c) {
        CHECK(fv::engine_filter(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

// Every text the inventory label shows: the count with a "." tail, then the runtime's tail. The
// vectors pin the runtimes (Auto means the CPU, the GPU build is the CUDA runtime, no Mac), which
// is the host these tests build.
TEST_CASE("ui_qt.dialogs.settings_models.engine_inventory vectors", "[golden]") {
    fv::golden::for_each(kModule, "engine_inventory", [](const Case& c) {
        const Json settings = c.arg<Json>(0);
        const auto cached = c.arg<std::vector<std::string>>(1);
        const auto installed = c.arg<std::vector<std::string>>(2);
        const std::set<std::string> models(cached.begin(), cached.end());
        const std::string backend = settings.at("selected_model").get<std::string>();
        std::vector<std::string> shown;
        if (!fv::engine_is_local(backend)) {
            shown.push_back(fv::engine_inventory_text(backend, models, "."));
        } else {
            const std::string device =
                settings.contains("local_asr_devices")
                    ? settings.at("local_asr_devices").value("parakeet", "auto")
                    : "auto";
            shown.push_back(fv::engine_inventory_text(backend, models, "."));
            shown.push_back(fv::engine_inventory_text(
                backend, models,
                fv::engine_runtime_label(
                    backend, device,
                    fvtest::hostWith(std::set<std::string>(installed.begin(), installed.end())))));
        }
        CHECK(shown == c.result<std::vector<std::string>>());
    });
}

// What no vector covers.

TEST_CASE("the device text of a local engine can be replaced", "[widget]") {
    const Json settings{{"selected_model", "parakeet"}};
    CHECK(fv::voice_detail(settings) == "On this computer · auto");
    CHECK(fv::voice_detail(settings, "GPU (RTX 2060)") == "On this computer · GPU (RTX 2060)");
    // Only a local engine shows a device; the others say where the audio goes.
    CHECK(fv::voice_detail(Json{{"selected_model", "remote"}}, "cpu") ==
          "Pair with a host in Remote engine");
}

TEST_CASE("the summary of the voice engine is the same from the rail and from the page",
          "[widget]") {
    // Rail, Home and the page read these functions; a saved name that is not an engine reads as
    // the default.
    CHECK(fv::voice_engine(Json::object()) == "parakeet");
    CHECK(fv::voice_engine(Json{{"selected_model", "remote"}}) == "remote");
    CHECK(fv::voice_engine(Json{{"selected_model", "local_whisper"}}) == "parakeet");
    CHECK(fv::voice_summary(Json{{"selected_model", "local_whisper"}}) == "Parakeet TDT 0.6B v3");
}

TEST_CASE("the inventory of an engine the catalog does not have is an error", "[widget]") {
    CHECK(fv::engine_inventory_count("api", {}).empty());
    CHECK(fv::engine_inventory_count("remote", {}).empty());
    CHECK_THROWS_AS(fv::engine_inventory_count("moonshine", {}), std::out_of_range);
    CHECK(fv::engine_inventory_count("parakeet", {"parakeet-v3"}) ==
          "1 of 2 Parakeet models on this computer");
    // A key that is not a model of the engine counts for nothing.
    CHECK(fv::engine_inventory_count("parakeet", {"whisper-base"}) ==
          "0 of 2 Parakeet models on this computer");
}

TEST_CASE("a runtime that cannot be resolved says where to look", "[widget]") {
    // Parakeet MLX has no GPU runtime to ask for here: resolving it fails, and so reads as
    // "could not be checked".
    CHECK(fv::engine_runtime_label("parakeet_mlx", "cuda", fvtest::hostWith({})) ==
          fv::kRuntimeCheckFailed);
    CHECK(fv::engine_runtime_label("parakeet", "cpu", fvtest::hostWith({"asr-nvidia-cpu"})) ==
          " · NVIDIA Speech CPU installed.");
    CHECK(fv::engine_runtime_label("parakeet", "cpu", fvtest::hostWith({})) ==
          " · NVIDIA Speech CPU not installed.");
}

// OpenWhisper_CPP's own runtime settings: the line names the runtime the engine will load.
TEST_CASE("the runtime line follows the runtime build and folder chosen", "[unit]") {
    const auto cpuOnly = fvtest::hostWith({"asr-nvidia-cpu"});
    Json settings{{"selected_model", "parakeet"}};
    // Nothing chosen: as OpenWhisper resolves the device.
    CHECK(fv::engine_runtime_label(settings, "parakeet", "auto", cpuOnly) ==
          " · NVIDIA Speech CPU installed.");
    // The GPU build chosen: it is the one loaded, even on the CPU.
    settings["fv_speech_runtime"] = "cuda";
    CHECK(fv::engine_runtime_label(settings, "parakeet", "cpu", cpuOnly) ==
          " · NVIDIA Speech GPU not installed.");
    settings["fv_speech_runtime"] = "cpu";
    CHECK(fv::engine_runtime_label(settings, "parakeet", "cuda", cpuOnly) ==
          " · NVIDIA Speech CPU installed.");
    // A runtime folder of the user's own stands in for the build it is.
    const fv::testkit::TempDataRoot root;
    const std::filesystem::path folder = root.path() / "nemo";
    std::filesystem::create_directories(folder / "bin");
    std::ofstream(folder / "bin" / fv::runtime_library_name()) << "runtime";
    std::ofstream(folder / "bin" / "ggml-cuda.dll") << "cuda";
    settings["fv_runtime_dir"] = fv::base_utf8::path_to_utf8(folder);
    settings["fv_speech_runtime"] = "";
    CHECK(fv::engine_runtime_label(settings, "parakeet", "cuda", fvtest::hostWith({})) ==
          " · NVIDIA Speech GPU installed.");
    // Another backend is not OpenWhisper_CPP's engine: its settings change nothing.
    CHECK(fv::engine_runtime_label(settings, "parakeet_mlx", "cuda", fvtest::hostWith({})) ==
          fv::kRuntimeCheckFailed);
}

TEST_CASE("the runtime summary says which build is loaded", "[widget]") {
    const Json none{{"selected_model", "parakeet"}};
    CHECK(fv::runtime_summary(none, true) == "Auto · no runtime yet");
    Json cpu = none;
    cpu["fv_speech_runtime"] = "cpu";
    CHECK(fv::runtime_summary(cpu, false) == "Auto · CPU runtime not loaded");
    CHECK(fv::runtime_summary(cpu, true) == "Auto · CPU runtime loaded");
}

TEST_CASE("the voice footnote and captions name only the engines OpenWhisper_CPP has", "[widget]") {
    for (const std::string_view text :
         {fv::kVoiceFootnote, fv::kApiEngineCaption, fv::kRemoteEngineCaption}) {
        for (const char* gone : {"Whisper", "Nemotron", "Moonshine", "Qwen"}) {
            CHECK(text.find(gone) == std::string_view::npos);
        }
    }
}
