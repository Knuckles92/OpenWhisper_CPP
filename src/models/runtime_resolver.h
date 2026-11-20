#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "base/json.h"
#include "models/catalog.h"

// Which speech runtime component and device a backend runs on (services/local_asr/catalog.py:
// RUNTIME_IDS, nvidia_gpu_runtime, apple_silicon, runtime_id, resolve_runtime, _auto_device,
// missing_runtime). The branches of the dropped backends are kept so a saved selection of one
// resolves as OpenWhisper resolves it, to a runtime this build's catalog does not have.
namespace fv::models {

// NVIDIA's CUDA release runs on Turing and newer; older cards run its Vulkan release (Linux).
inline constexpr std::pair<int, int> kCudaMinComputeCapability{7, 5};

// The speech runtime ids this build's catalog may hold, in Downloads order.
std::span<const std::string_view> runtime_ids();

// The NVIDIA GPU runtime for this computer's card: the Vulkan release for a card older than
// Turing where the platform publishes it, else the CUDA release (also for an unknown
// capability).
std::string nvidia_gpu_runtime(const Host& host);

// An Apple Silicon Mac, where Auto means the Apple GPU, never CUDA.
bool apple_silicon(const Host& host);

// The runtime component `backend` uses on `device` ("cpu", "cuda", "metal", ...).
std::string runtime_id(std::string_view backend, std::string_view device, const Host& host);

// The device Auto runs on once its runtime is installed (not Apple Silicon). OpenWhisper counted
// CUDA devices through CTranslate2, which needs its GPU Acceleration component; FioriVoce asks
// NVML whether an NVIDIA GPU is present. A card the Vulkan runtime serves also means "cuda".
std::string auto_device(std::string_view backend, const Host& host);

// (component, device) for a requested device ("auto", "cpu", "cuda"). Auto falls back to the CPU
// runtime while the GPU one is not installed (Parakeet, Nemotron); an explicit GPU never does.
// Throws std::invalid_argument "Parakeet MLX supports Auto (Apple GPU) or CPU, not CUDA." for
// Parakeet MLX with any other request.
std::pair<std::string, std::string> resolve_runtime(std::string_view backend,
                                                    std::string_view requested, const Host& host);

// The runtime the selected device of `model_name` needs when it is published, not installed and
// can run here; nullopt otherwise (and for a model not in the catalog).
std::optional<std::string> missing_runtime(std::string_view model_name, const Json& settings,
                                           const Host& host);

} // namespace fv::models
