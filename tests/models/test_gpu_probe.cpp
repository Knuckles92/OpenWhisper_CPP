// The NVIDIA GPU probe: OpenWhisper's tests/test_gpu_info.py keeps one test that is not about
// Whisper's VRAM planning; the rest checks query_nvml against a scripted NVML.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "models/gpu_probe.h"

using namespace fv::models;

namespace {

// A scripted NVML: one device unless `devices` says otherwise.
struct Script {
    int init = 0;
    unsigned int devices = 1;
    const char* name = "  NVIDIA GeForce RTX 2060  ";
    unsigned long long total = 6ULL * 1024 * 1024 * 1024 + 123;
    bool capability = true;
    int shutdowns = 0;
};
Script* script = nullptr;

NvmlApi scripted(Script& s) {
    script = &s;
    NvmlApi api;
    api.init = [] {
        return script->init;
    };
    api.shutdown = [] {
        ++script->shutdowns;
        return 0;
    };
    api.get_count = [](unsigned int* count) {
        *count = script->devices;
        return 0;
    };
    api.get_handle = [](unsigned int, void** handle) {
        *handle = script;
        return 0;
    };
    api.get_name = [](void*, char* buffer, unsigned int size) {
        const std::size_t length = std::min<std::size_t>(std::strlen(script->name), size - 1);
        std::memcpy(buffer, script->name, length);
        buffer[length] = 0;
        return 0;
    };
    api.get_memory = [](void*, NvmlApi::Memory* memory) {
        memory->total = script->total;
        return 0;
    };
    if (s.capability) {
        api.get_compute_capability = [](void*, int* major, int* minor) {
            *major = 7;
            *minor = 5;
            return 0;
        };
    }
    return api;
}

} // namespace

TEST_CASE("test_no_nvml_means_no_gpu", "[test_gpu_info]") {
    GpuProbe probe([] { return std::optional<NvmlApi>{}; });
    CHECK_FALSE(probe.gpu());
}

TEST_CASE("NVML's first device is read once, stripped, in MiB", "[models]") {
    Script s;
    int loads = 0;
    GpuProbe probe([&] {
        ++loads;
        return std::optional{scripted(s)};
    });
    const auto card = probe.gpu();
    REQUIRE(card);
    CHECK(card->name == "NVIDIA GeForce RTX 2060");
    CHECK(card->total_mib == 6144);
    CHECK(card->compute_capability == std::pair{7, 5});
    CHECK(card->short_name() == "RTX 2060");
    CHECK(s.shutdowns == 1);
    CHECK(probe.gpu() == card);
    CHECK(loads == 1);
}

TEST_CASE("NVML without a device, a failed init or no capability", "[models]") {
    Script none;
    none.devices = 0;
    CHECK_FALSE(query_nvml(scripted(none)));
    CHECK(none.shutdowns == 1);

    Script failed;
    failed.init = 9;
    CHECK_FALSE(query_nvml(scripted(failed)));
    CHECK(failed.shutdowns == 0);

    Script older;
    older.capability = false;
    const auto card = query_nvml(scripted(older));
    REQUIRE(card);
    CHECK_FALSE(card->compute_capability);
}

TEST_CASE("a loader that throws reads as no GPU", "[models]") {
    GpuProbe probe([]() -> std::optional<NvmlApi> { throw std::runtime_error("boom"); });
    CHECK_FALSE(probe.gpu());
}

TEST_CASE("short names and GB text", "[models]") {
    NvidiaGpu card;
    card.name = "NVIDIA GeForce GTX 1050 Ti";
    CHECK(card.short_name() == "GTX 1050 Ti");
    card.name = "Quadro RTX 4000";
    CHECK(card.short_name() == "RTX 4000");
    card.name = "NVIDIA ";
    CHECK(card.short_name() == "NVIDIA ");
    CHECK(format_gb(4096) == "4.0 GB");
    CHECK(format_gb(6144 + 51) == "6.0 GB");
}

TEST_CASE("the real NVML query never throws", "[models][platform]") {
    // Reads this computer's driver, if any; changes nothing.
    CHECK_NOTHROW(nvidia_gpu());
}
