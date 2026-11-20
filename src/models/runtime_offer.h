#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "base/json.h"
#include "models/catalog.h"

// The GPU runtime offered beside Auto's CPU fallback on first use
// (services/local_asr/catalog.py:gpu_runtime_offer). Auto runs Parakeet on an installed GPU
// runtime, else on the CPU one, so missing_runtime asks a fresh install for the CPU runtime even
// where the NVIDIA GPU would run the model several times faster; this names the runtime Auto
// would pick once installed.
// Affinity: thread-safe (pure over its arguments).
namespace fv::models {

// Whether the Vulkan loader (libvulkan.so.1) loads on this computer.
bool vulkan_loader_present();

struct RuntimeOfferProbes {
    const Json* catalog = nullptr;               // nullptr: builtin_catalog()
    std::function<bool()> vulkan_loader_present; // empty: models::vulkan_loader_present
};

// The runtime Auto would pick once installed (CUDA, or Vulkan for an older card on Linux with the
// Vulkan loader), while no NVIDIA Speech runtime is installed and this computer can load it.
// nullopt otherwise: for a model not in the catalog or of another backend, on Apple Silicon, for
// an explicit CPU or CUDA choice (missing_runtime answers those), once the CPU runtime is
// installed, without an NVIDIA GPU, for a CUDA card older than Turing or of unknown capability,
// and for a runtime already installed, unpublished or incompatible here.
std::optional<std::string> gpu_runtime_offer(std::string_view model_name, const Json& settings,
                                             const Host& host,
                                             const RuntimeOfferProbes& probes = {});

} // namespace fv::models
