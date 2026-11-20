// The speech catalog and runtime resolution: the resolver half of OpenWhisper's
// tests/test_local_asr.py, and the Parakeet engine's resolution messages
// (transcriber/optional_backend.py reload_model) through resolve_parakeet. OpenWhisper's CUDA
// device count came from CTranslate2; here NVML's GPU stands for it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <set>
#include <string>

#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "engine/runtime.h"
#include "models/catalog.h"
#include "models/engine_resolution.h"
#include "models/runtime_resolver.h"
#include "models/speech_catalog.h"
#include "models/testing/fake_openwhisper.h"
#include "models_test_support.h"
#include "pyport/json.h"

using namespace fv::models;
using namespace fv::models::test;
using Catch::Matchers::ContainsSubstring;
using fv::Json;

namespace {

using Installed = std::set<std::string, std::less<>>;

Host host_with(std::string platform, const Installed& installed,
               std::optional<NvidiaGpu> card = std::nullopt) {
    Host host = host_for(std::move(platform), std::move(card));
    host.is_installed = [&installed](std::string_view id) {
        return installed.contains(id);
    };
    return host;
}

// A data folder in a temporary LOCALAPPDATA, so components and speech-models are test folders.
struct TempPaths {
    TempDir temp;
    fv::MapEnvReader env{{"LOCALAPPDATA", fv::base_utf8::path_to_utf8(temp.path())},
                         {"USERPROFILE", fv::base_utf8::path_to_utf8(temp.path())},
                         {"HOME", fv::base_utf8::path_to_utf8(temp.path())},
                         {"XDG_DATA_HOME", fv::base_utf8::path_to_utf8(temp.path())}};
    fv::Paths paths = fv::Paths::resolve(fv::identity(), env);
};

} // namespace

TEST_CASE("test_every_optional_artifact_is_pinned_and_has_integrity_metadata", "[test_local_asr]") {
    // Only the Parakeet family is offered: one backend, Parakeet TDT v3 and Orukeet.
    CHECK(backends().size() == 1);
    CHECK(models().size() == 2);
    CHECK(model("orukeet-v0.1") != nullptr);
    for (const auto& entry : models()) {
        const Json& spec = artifacts(entry.key);
        CHECK_FALSE(spec.at("revision").get<std::string>().empty());
        for (const Json& file : spec.at("files")) {
            CHECK(file.at("sha256").get<std::string>().size() == 64);
            CHECK(file.at("size_bytes").get<std::int64_t>() > 0);
            CHECK(file.at("url").get<std::string>().starts_with("https://"));
            const auto name = file.at("name").get<std::string>();
            CHECK(fs::path(name).filename().string() == name);
        }
    }
    const Json& components = builtin_catalog().at("components");
    std::set<std::string> ids;
    for (const auto& [id, runtime] : components.items()) {
        ids.insert(id);
        for (const auto& [platform, entry] : runtime.at("platforms").items()) {
            CHECK(entry.at("platform") == platform);
            for (const Json& archive : entry.at("archives")) {
                CHECK(archive.at("sha256").get<std::string>().size() == 64);
                CHECK(archive.at("size_bytes").get<std::int64_t>() > 0);
                CHECK(archive.at("url").get<std::string>().starts_with("https://github.com/"));
            }
        }
    }
    CHECK(ids == std::set<std::string>(runtime_ids().begin(), runtime_ids().end()));
}

TEST_CASE("test_linux_x86_64_offers_only_the_native_runtimes", "[test_local_asr]") {
    const Json& components = builtin_catalog().at("components");
    std::set<std::string> linux;
    for (const auto& [id, runtime] : components.items()) {
        if (runtime.at("platforms").contains("linux_x86_64")) {
            linux.insert(id);
        }
    }
    CHECK(linux == std::set<std::string>{"asr-nvidia-cpu", "asr-nvidia-cuda", "asr-nvidia-vulkan"});
    CHECK(components.at("asr-nvidia-vulkan").at("platforms").size() == 1);
    for (const auto& [key, flavor] :
         {std::pair{"asr-nvidia-cpu", "cpu"}, std::pair{"asr-nvidia-cuda", "cuda"},
          std::pair{"asr-nvidia-vulkan", "vulkan"}}) {
        const Json& archives = components.at(key).at("platforms").at("linux_x86_64").at("archives");
        REQUIRE(archives.size() == 1);
        const Json& archive = archives[0];
        CHECK(archive.at("extract") == "nemo-tar");
        CHECK(archive.at("root") == std::string("nemo-speech-0.1.0-linux-x86_64-") + flavor);
        CHECK(archive.at("name") == archive.at("root").get<std::string>() + ".tar.gz");
        CHECK(archive.at("url").get<std::string>().ends_with(
            "/v0.1.0/" + archive.at("name").get<std::string>()));
    }
}

TEST_CASE("test_linux_parakeet_offers_cpu_and_gpu_runtimes", "[test_local_asr]") {
    TempPaths temp;
    const Json settings{{"local_asr_devices", {{"parakeet", "cuda"}}}};
    const auto resolved = resolve_parakeet(settings, temp.paths, host_for("linux_x86_64"));
    CHECK(resolved.runtime_id == "asr-nvidia-cuda");
    REQUIRE(resolved.missing_message);
    CHECK_THAT(*resolved.missing_message, ContainsSubstring("GPU runtime in Downloads"));
    CHECK(resolved.missing_state == EngineMissing::RuntimeMissing);
}

TEST_CASE("test_only_pre_turing_gpus_run_the_vulkan_runtime", "[test_local_asr]") {
    const Installed none;
    for (const auto& [card, expected] :
         {std::pair{std::optional<NvidiaGpu>{}, "asr-nvidia-cuda"},
          std::pair{std::optional{gpu("NVIDIA GeForce RTX 2060", 6144, {{7, 5}})},
                    "asr-nvidia-cuda"},
          std::pair{std::optional{gpu("NVIDIA GeForce GTX 1050 Ti", 4096, std::nullopt)},
                    "asr-nvidia-cuda"},
          std::pair{std::optional{gpu("NVIDIA GeForce GTX 1050 Ti", 4096, {{6, 1}})},
                    "asr-nvidia-vulkan"}}) {
        const Host host = host_with("linux_x86_64", none, card);
        CHECK(runtime_id("parakeet", "cuda", host) == expected);
        CHECK(runtime_id("nemotron", "cuda", host) == expected);
        CHECK(runtime_id("parakeet", "cpu", host) == "asr-nvidia-cpu");
        CHECK(runtime_id("qwen_asr", "cuda", host) == "asr-qwen");
    }
    // Without a Vulkan release for the platform, the card keeps CUDA.
    const Host windows = host_with("win_amd64", none, gpu("GTX 1050 Ti", 4096, {{6, 1}}));
    CHECK(runtime_id("parakeet", "cuda", windows) == "asr-nvidia-cuda");
}

TEST_CASE("test_auto_uses_the_vulkan_runtime_only_once_installed", "[test_local_asr]") {
    Installed installed{"asr-nvidia-cpu"};
    using P = std::pair<std::string, std::string>;
    // The Vulkan runtime needs only the card.
    Host host = host_with("linux_x86_64", installed, gpu("GTX 1050 Ti", 4096, {{6, 1}}));
    CHECK(resolve_runtime("parakeet", "auto", host) == P{"asr-nvidia-cpu", "cpu"});
    CHECK(resolve_runtime("parakeet", "cuda", host) == P{"asr-nvidia-vulkan", "cuda"});
    installed.emplace("asr-nvidia-vulkan");
    CHECK(resolve_runtime("nemotron", "auto", host) == P{"asr-nvidia-vulkan", "cuda"});
    CHECK(resolve_runtime("nemotron", "cpu", host) == P{"asr-nvidia-cpu", "cpu"});
    // A Turing card: Auto uses the CUDA runtime once it is installed, the CPU one until then.
    installed.erase("asr-nvidia-vulkan");
    Host turing = host_with("linux_x86_64", installed, gpu("RTX 2060", 6144, {{7, 5}}));
    CHECK(resolve_runtime("parakeet", "auto", turing) == P{"asr-nvidia-cpu", "cpu"});
    installed.emplace("asr-nvidia-cuda");
    CHECK(resolve_runtime("parakeet", "auto", turing) == P{"asr-nvidia-cuda", "cuda"});
    // No NVIDIA GPU at all (OpenWhisper: CTranslate2 counts no CUDA device): Auto stays on the
    // CPU even with the CUDA runtime installed.
    Host no_gpu = host_with("linux_x86_64", installed);
    CHECK(resolve_runtime("parakeet", "auto", no_gpu) == P{"asr-nvidia-cpu", "cpu"});
}

TEST_CASE("test_explicit_devices_keep_their_own_runtime", "[test_local_asr]") {
    const Installed none;
    const Host host = host_with("win_amd64", none, gpu("NVIDIA GeForce RTX 2060", 6144, {{7, 5}}));
    const Json cpu{{"local_asr_devices", {{"parakeet", "cpu"}}}};
    const Json cuda{{"local_asr_devices", {{"parakeet", "cuda"}}}};
    CHECK(missing_runtime("parakeet-v3", cpu, host) == "asr-nvidia-cpu");
    CHECK(missing_runtime("parakeet-v3", cuda, host) == "asr-nvidia-cuda");
}

TEST_CASE("test_settings_never_cross_model_families_or_change_whisper", "[test_local_asr]") {
    const Json settings{{"local_asr_models", {{"parakeet", "qwen-1.7b"}}},
                        {"whisper_model", "turbo"}};
    CHECK(selected_model(settings, "parakeet") == "parakeet-v3");
    CHECK(settings.at("whisper_model") == "turbo");
    CHECK(selected_device(Json{{"local_asr_devices", {{"moonshine", "cuda"}}}}, "moonshine") ==
          "cpu");
    CHECK(selected_device(Json{{"local_asr_devices", Json::array()}}, "qwen_asr") == "auto");
}

TEST_CASE("test_auto_can_use_cpu_runtime_but_explicit_cuda_cannot", "[test_local_asr]") {
    TempPaths temp;
    // The CPU runtime is installed, the model is not downloaded.
    write_file(temp.paths.components_root() / "asr-nvidia-cpu" / ".installed", "0.1.0-py312");
    const Host windows = host_for("win_amd64", gpu("RTX 2060", 6144, {{7, 5}}));
    const auto automatic = resolve_parakeet(Json::object(), temp.paths, windows);
    CHECK(automatic.runtime_id == "asr-nvidia-cpu");
    CHECK(automatic.missing_state == EngineMissing::ModelMissing);
    const auto explicit_cuda =
        resolve_parakeet(Json{{"local_asr_devices", {{"parakeet", "cuda"}}}}, temp.paths, windows);
    CHECK(explicit_cuda.runtime_id == "asr-nvidia-cuda");
    REQUIRE(explicit_cuda.missing_message);
    CHECK_THAT(*explicit_cuda.missing_message, ContainsSubstring("GPU"));
}

TEST_CASE("test_unsupported_runtime_does_not_point_to_downloads", "[test_local_asr]") {
    // Linux ARM64 has no Parakeet runtime (OpenWhisper's test asks this of Qwen there).
    TempPaths temp;
    const auto resolved = resolve_parakeet(Json{{"local_asr_devices", {{"parakeet", "cpu"}}}},
                                           temp.paths, host_for("linux_aarch64"));
    CHECK(resolved.runtime_id.empty());
    REQUIRE(resolved.missing_message);
    CHECK_THAT(*resolved.missing_message, ContainsSubstring("not available on this platform"));
    CHECK_THAT(*resolved.missing_message, !ContainsSubstring("Downloads"));
}

// OpenWhisper_CPP: the resolution messages and paths resolve_parakeet hands the speech service.

TEST_CASE("resolve_parakeet words each missing piece as OpenWhisper does", "[models]") {
    TempPaths temp;
    const Host windows = host_for("win_amd64");
    auto resolved = resolve_parakeet(Json::object(), temp.paths, windows);
    CHECK(resolved.missing_message == "Install Parakeet's CPU runtime in Downloads.");
    CHECK(resolved.device == "cpu");
    CHECK(resolved.gpu_index == -1);

    resolved = resolve_parakeet(Json::object(), temp.paths, host_for("linux_aarch64"));
    CHECK(resolved.missing_message == "Parakeet's CPU runtime is not available on this platform.");

    write_file(temp.paths.components_root() / "asr-nvidia-cpu" / ".installed", "x");
    resolved = resolve_parakeet(Json{{"local_asr_models", {{"parakeet", "orukeet-v0.1"}}}},
                                temp.paths, windows);
    CHECK(resolved.missing_message == "Download Orukeet TDT 0.6B in Downloads.");
    CHECK(resolved.model_key == "orukeet-v0.1");
    REQUIRE(resolved.runtime_dir);
    CHECK(*resolved.runtime_dir == temp.paths.components_root() / "asr-nvidia-cpu");
}

TEST_CASE("resolve_parakeet finds the installed runtime and the pinned model file", "[models]") {
    TempPaths temp;
    // A model folder whose installed.json and sizes match the real pin: the file is sparse-size
    // only, which is all resolution checks.
    const Json& spec = artifacts("parakeet-v3");
    const auto folder = temp.paths.speech_models_root() / "parakeet-v3";
    const auto name = spec.at("files")[0].at("name").get<std::string>();
    write_file(folder / name, "");
    fs::resize_file(folder / name, spec.at("files")[0].at("size_bytes").get<std::uint64_t>());
    write_file(folder / "installed.json", fv::py::dumps(spec));

    const auto model_only = resolve_parakeet(Json::object(), temp.paths, host_for("win_amd64"));
    CHECK_FALSE(model_only.usable());
    CHECK_FALSE(model_only.runtime_dir);
    CHECK(model_only.model_path == folder / name);
    CHECK(model_only.missing_state == EngineMissing::RuntimeMissing);
    CHECK(model_only.missing_message == "Install Parakeet's CPU runtime in Downloads.");

    write_file(temp.paths.components_root() / "asr-nvidia-cuda" / ".installed", "x");
    const auto resolved =
        resolve_parakeet(Json::object(), temp.paths,
                         host_for("win_amd64", gpu("NVIDIA GeForce RTX 2060", 6144, {{7, 5}})));
    CHECK_FALSE(resolved.missing_message);
    CHECK(resolved.usable());
    CHECK(resolved.device == "cuda");
    CHECK(resolved.requested_device == "auto");
    CHECK(resolved.gpu_index == 0);
    CHECK(*resolved.model_path == folder / name);
    CHECK(resolved.model_label == "Parakeet TDT 0.6B v3");
}

TEST_CASE("fv_speech_runtime fixes the runtime build and fv_runtime_dir stands in for it",
          "[models]") {
    TempPaths temp;
    const Host windows = host_for("win_amd64", gpu("NVIDIA GeForce RTX 2060", 6144, {{7, 5}}));
    write_file(temp.paths.components_root() / "asr-nvidia-cuda" / ".installed", "x");
    // The crash offer saved "cpu": the CPU runtime, which is not installed.
    auto resolved = resolve_parakeet(Json{{"fv_speech_runtime", "cpu"}}, temp.paths, windows);
    CHECK(resolved.runtime_id == "asr-nvidia-cpu");
    CHECK(resolved.device == "cpu");
    CHECK(resolved.missing_message == "Install Parakeet's CPU runtime in Downloads.");
    // "cuda" with the device set to CPU: the CUDA build, run on the CPU.
    resolved = resolve_parakeet(
        Json{{"fv_speech_runtime", "cuda"}, {"local_asr_devices", {{"parakeet", "cpu"}}}},
        temp.paths, windows);
    CHECK(resolved.runtime_id == "asr-nvidia-cuda");
    CHECK(resolved.device == "cpu");
    CHECK(resolved.missing_state == EngineMissing::ModelMissing);
    // An explicit folder replaces the component when it holds a runtime: here a CPU build, so
    // Auto keeps the CPU.
    const auto chosen = temp.temp.path() / "my-runtime";
    write_file(chosen / "bin" / fv::runtime_library_name(), "runtime");
    resolved = resolve_parakeet(Json{{"fv_runtime_dir", fv::base_utf8::path_to_utf8(chosen)}},
                                temp.paths, windows);
    REQUIRE(resolved.runtime_dir);
    CHECK(*resolved.runtime_dir == chosen);
    CHECK(resolved.missing_state == EngineMissing::ModelMissing);
}

TEST_CASE("backend choices hold Parakeet only", "[models]") {
    const auto choices = backend_choices();
    REQUIRE(choices.size() == 1);
    CHECK(choices[0].label == "Parakeet");
    CHECK(choices[0].value == "parakeet");
    CHECK(backend_of("parakeet-v3") == "parakeet");
    CHECK(backend_of("turbo") == kWhisperBackend);
    CHECK(default_model("parakeet") == "parakeet-v3");
    CHECK_THROWS_AS(default_model("qwen_asr"), std::out_of_range);
}
