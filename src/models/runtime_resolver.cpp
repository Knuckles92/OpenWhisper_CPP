#include "models/runtime_resolver.h"

#include <array>
#include <stdexcept>

#include "models/speech_catalog.h"

namespace fv::models {

namespace {

constexpr std::string_view kCpuRuntime = component_id::kAsrNvidiaCpu;
constexpr std::string_view kCudaRuntime = component_id::kAsrNvidiaCuda;
constexpr std::string_view kVulkanRuntime = component_id::kAsrNvidiaVulkan;
constexpr std::string_view kMetalRuntime = component_id::kAsrNvidiaMetal;
// Runtimes of backends OpenWhisper_CPP does not have; no catalog entry names them.
constexpr std::string_view kMlxRuntime = "asr-parakeet-mlx";
constexpr std::string_view kTranscribeRuntime = "asr-transcribe";

constexpr std::array<std::string_view, 4> kRuntimeIds{kCpuRuntime, kCudaRuntime, kVulkanRuntime,
                                                      kMetalRuntime};

bool nvidia_family(std::string_view backend) {
    return backend == "parakeet" || backend == "nemotron";
}

} // namespace

std::span<const std::string_view> runtime_ids() {
    return kRuntimeIds;
}

std::string nvidia_gpu_runtime(const Host& host) {
    const auto gpu = host.nvidia();
    std::optional<std::pair<int, int>> capability;
    if (gpu) {
        capability = gpu->compute_capability;
    }
    if (capability && *capability < kCudaMinComputeCapability &&
        component_is_published(kVulkanRuntime, host.platform.value_or(std::string()))) {
        return std::string(kVulkanRuntime);
    }
    return std::string(kCudaRuntime);
}

bool apple_silicon(const Host& host) {
    return host.platform == platform::kDarwinArm64;
}

std::string runtime_id(std::string_view backend, std::string_view device, const Host& host) {
    if (backend == "parakeet_mlx") {
        return std::string(kMlxRuntime);
    }
    if (backend == "parakeet_unified") {
        return std::string(kTranscribeRuntime);
    }
    if (backend == "qwen_asr") {
        return "asr-qwen";
    }
    if (backend == "moonshine") {
        return "asr-moonshine";
    }
    if (device != "cpu" && apple_silicon(host)) {
        return std::string(kMetalRuntime);
    }
    return device == "cuda" ? nvidia_gpu_runtime(host) : std::string(kCpuRuntime);
}

std::string auto_device(std::string_view backend, const Host& host) {
    std::string device = host.nvidia() ? "cuda" : "cpu";
    // The Vulkan runtime needs only the card.
    if (device == "cpu" && nvidia_family(backend) &&
        runtime_id(backend, "cuda", host) == kVulkanRuntime) {
        device = "cuda";
    }
    return device;
}

std::pair<std::string, std::string> resolve_runtime(std::string_view backend,
                                                    std::string_view requested, const Host& host) {
    if (backend == "parakeet_mlx") {
        if (requested != "auto" && requested != "cpu") {
            throw std::invalid_argument("Parakeet MLX supports Auto (Apple GPU) or CPU, not CUDA.");
        }
        return {std::string(kMlxRuntime), requested == "cpu" ? "cpu" : "metal"};
    }
    if (backend == "parakeet_unified") {
        return {std::string(kTranscribeRuntime), std::string(requested)};
    }
    if ((backend == "qwen_asr" || nvidia_family(backend)) && apple_silicon(host)) {
        // A saved "cuda" from another computer means the Apple GPU here too.
        if (requested == "cpu") {
            return {runtime_id(backend, "cpu", host), "cpu"};
        }
        if (backend == "qwen_asr") {
            return {"asr-qwen", "mps"};
        }
        // Auto keeps an installed CPU runtime until the Metal one is added.
        if (requested == "auto" && host.installed(kCpuRuntime) && !host.installed(kMetalRuntime)) {
            return {std::string(kCpuRuntime), "cpu"};
        }
        return {std::string(kMetalRuntime), "metal"};
    }
    std::string device = requested == "auto" ? auto_device(backend, host) : std::string(requested);
    std::string component = runtime_id(backend, device, host);
    // Explicit CUDA must never silently fall back to the CPU.
    if (requested == "auto" && !host.installed(component) && nvidia_family(backend)) {
        component = runtime_id(backend, "cpu", host);
        device = "cpu";
    }
    return {component, device};
}

std::optional<std::string> missing_runtime(std::string_view model_name, const Json& settings,
                                           const Host& host) {
    const SpeechModel* found = model(model_name);
    if (found == nullptr) {
        return std::nullopt;
    }
    const auto [component, device] =
        resolve_runtime(found->backend, selected_device(settings, found->backend), host);
    const std::string tag = host.platform.value_or(std::string());
    if (!component_is_published(component, tag) || host.installed(component)) {
        return std::nullopt;
    }
    // Never offer a runtime this computer cannot load.
    const auto entry = catalog_entry(component, tag);
    if (check_compatibility(entry.value_or(Json::object()), host)) {
        return std::nullopt;
    }
    return component;
}

} // namespace fv::models
