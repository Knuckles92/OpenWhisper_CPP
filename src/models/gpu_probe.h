#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// What this computer's NVIDIA GPU is (services/gpu_info.py:50-160 and format_gb), asked of NVML,
// which ships with the NVIDIA driver. The Whisper VRAM planning of gpu_info.py is not ported.
namespace fv::models {

// The first NVIDIA GPU, the one the CUDA runtime uses by default.
struct NvidiaGpu {
    std::string name;
    std::int64_t total_mib = 0;
    // (major, minor), e.g. (6, 1) for Pascal's GTX 10 series; NVML may not report it.
    std::optional<std::pair<int, int>> compute_capability;

    // "GTX 1050 Ti" rather than "NVIDIA GeForce GTX 1050 Ti": "NVIDIA ", "GeForce " and
    // "Quadro " are removed in that order where they lead; the full name if nothing is left.
    std::string short_name() const;

    bool operator==(const NvidiaGpu&) const = default;
};

// "{mib / 1024:.1f} GB".
std::string format_gb(std::int64_t mib);

// The NVML entry points _query_nvml calls, as loaded from a library. Every pointer but
// get_compute_capability is required; v2 symbols are preferred where the library has them.
struct NvmlApi {
    using Init = int (*)();
    using Shutdown = int (*)();
    using GetCount = int (*)(unsigned int*);
    using GetHandle = int (*)(unsigned int, void**);
    using GetName = int (*)(void*, char*, unsigned int);
    struct Memory {
        unsigned long long total, free, used;
    };
    using GetMemory = int (*)(void*, Memory*);
    using GetCapability = int (*)(void*, int*, int*);

    Init init = nullptr;
    Shutdown shutdown = nullptr;
    GetCount get_count = nullptr;
    GetHandle get_handle = nullptr;
    GetName get_name = nullptr;
    GetMemory get_memory = nullptr;
    GetCapability get_compute_capability = nullptr;
    std::shared_ptr<void> keep_loaded; // the library, held while the pointers are used
};

// The NVML library paths tried in order (_nvml_candidates): on Windows System32's nvml.dll, then
// %ProgramFiles%\NVIDIA Corporation\NVSMI\nvml.dll; on Linux libnvidia-ml.so.1, then
// libnvidia-ml.so; nothing elsewhere.
std::vector<std::filesystem::path> nvml_candidates();

// _load_nvml: the first candidate that loads with every required symbol, else nullopt.
std::optional<NvmlApi> load_nvml();

// _query_nvml over `api`: nullopt when init fails, there is no device, or a required query
// fails; nvmlShutdown always follows a successful init. The name is decoded as UTF-8 (invalid
// bytes replaced) and stripped; total_mib is total bytes // (1024 * 1024).
std::optional<NvidiaGpu> query_nvml(const NvmlApi& api);

// nvidia_gpu's cache: the first answer of `loader` (load_nvml when empty), queried once, under a
// lock. Never throws: a loader or query that throws reads as no GPU.
// Affinity: thread-safe.
class GpuProbe {
public:
    using Loader = std::function<std::optional<NvmlApi>()>;
    explicit GpuProbe(Loader loader = {});
    std::optional<NvidiaGpu> gpu();

private:
    struct State;
    std::shared_ptr<State> state_;
};

// The first NVIDIA GPU, or nullopt without one (or without its driver). Asked once per process;
// the hardware does not change while the app runs. Never throws.
std::optional<NvidiaGpu> nvidia_gpu();

} // namespace fv::models
