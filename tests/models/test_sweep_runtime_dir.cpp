// fv_runtime_dir: a NeMo-Speech.cpp folder the user chose stands in for the runtime component
// only when it is one, and only for the build it is.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "engine/runtime.h"
#include "models/engine_resolution.h"
#include "models_test_support.h"
#include "pyport/json.h"

namespace fs = std::filesystem;
using namespace fv::models;
using namespace fv::models::test;
using fv::Json;

namespace {

struct TempPaths {
    TempDir temp;
    fv::MapEnvReader env{{"LOCALAPPDATA", fv::base_utf8::path_to_utf8(temp.path())},
                         {"USERPROFILE", fv::base_utf8::path_to_utf8(temp.path())},
                         {"HOME", fv::base_utf8::path_to_utf8(temp.path())},
                         {"XDG_DATA_HOME", fv::base_utf8::path_to_utf8(temp.path())}};
    fv::Paths paths = fv::Paths::resolve(fv::identity(), env);
};

// A release folder: the runtime library, and a ggml GPU backend beside it for a GPU build.
fs::path runtime_folder(const fs::path& where, bool gpu) {
    write_file(where / "bin" / fv::runtime_library_name(), "runtime");
    write_file(where / "bin" / "ggml-cpu.dll", "cpu");
    if (gpu) {
        write_file(where / "bin" / "ggml-cuda.dll", "cuda");
    }
    return where;
}

Json chose(const fs::path& folder, const char* device = "auto") {
    return Json{{"fv_runtime_dir", fv::base_utf8::path_to_utf8(folder)},
                {"local_asr_devices", {{"parakeet", device}}}};
}

const Host kWindowsWithGpu = host_for("win_amd64", gpu("NVIDIA GeForce RTX 2060", 6144, {{7, 5}}));

} // namespace

TEST_CASE("runtime_dir_serves needs the library, and a GPU build for a GPU runtime", "[models]") {
    TempDir temp;
    const fs::path empty = temp.path() / "empty";
    fs::create_directories(empty);
    CHECK_FALSE(runtime_dir_serves(empty, "asr-nvidia-cpu"));
    CHECK_FALSE(runtime_dir_serves(temp.path() / "missing", "asr-nvidia-cpu"));
    const fs::path cpu = runtime_folder(temp.path() / "cpu", false);
    CHECK(runtime_dir_serves(cpu, "asr-nvidia-cpu"));
    CHECK_FALSE(runtime_dir_serves(cpu, "asr-nvidia-cuda"));
    const fs::path cuda = runtime_folder(temp.path() / "cuda", true);
    CHECK(runtime_dir_serves(cuda, "asr-nvidia-cpu"));
    CHECK(runtime_dir_serves(cuda, "asr-nvidia-cuda"));
    CHECK(fv::runtime_has_gpu_backend(cuda));
    CHECK_FALSE(fv::runtime_has_gpu_backend(cpu));
}

TEST_CASE("a chosen folder without the runtime library is a missing runtime", "[models]") {
    TempPaths temp;
    const fs::path folder = temp.temp.path() / "not-a-runtime";
    fs::create_directories(folder);
    const auto resolved = resolve_parakeet(chose(folder), temp.paths, kWindowsWithGpu);
    CHECK_FALSE(resolved.runtime_dir);
    CHECK(resolved.missing_state == EngineMissing::RuntimeMissing);
    CHECK(resolved.missing_message == "No speech runtime in " +
                                          fv::base_utf8::path_to_utf8(folder) + ": " +
                                          fv::runtime_library_name() + " was not found.");
}

TEST_CASE("a chosen CPU build keeps Auto on the CPU and refuses an explicit GPU", "[models]") {
    TempPaths temp;
    const fs::path folder = runtime_folder(temp.temp.path() / "cpu-build", false);

    auto resolved = resolve_parakeet(chose(folder), temp.paths, kWindowsWithGpu);
    REQUIRE(resolved.runtime_dir);
    CHECK(*resolved.runtime_dir == folder);
    CHECK(resolved.device == "cpu");
    CHECK(resolved.runtime_id == "asr-nvidia-cpu");

    resolved = resolve_parakeet(chose(folder, "cuda"), temp.paths, kWindowsWithGpu);
    CHECK_FALSE(resolved.runtime_dir);
    CHECK(resolved.missing_state == EngineMissing::RuntimeMissing);
    CHECK(resolved.missing_message == fv::base_utf8::path_to_utf8(folder) +
                                          " holds a CPU build of NeMo-Speech.cpp, so Parakeet "
                                          "can't use the GPU with it.");

    // A GPU build serves the GPU.
    const fs::path gpu_build = runtime_folder(temp.temp.path() / "gpu-build", true);
    resolved = resolve_parakeet(chose(gpu_build, "cuda"), temp.paths, kWindowsWithGpu);
    REQUIRE(resolved.runtime_dir);
    CHECK(*resolved.runtime_dir == gpu_build);
    CHECK(resolved.device == "cuda");
    resolved = resolve_parakeet(chose(gpu_build), temp.paths, kWindowsWithGpu);
    CHECK(resolved.device == "cuda");
}
