#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "base/json.h"
#include "base/paths.h"
#include "models/catalog.h"

// Which runtime folder, model file and device the Parakeet engine loads, or why it cannot
// (transcriber/optional_backend.py:100-153 reload_model, _explain_missing_runtime and
// _runtime_kind: the resolution half; loading is fv_speech's).
namespace fv::models {

// What is missing, for speech::ReadyState (RuntimeMissing, ModelMissing).
enum class EngineMissing { None, RuntimeMissing, ModelMissing };

struct ResolvedEngine {
    std::optional<std::filesystem::path> runtime_dir; // the runtime component's folder
    std::optional<std::filesystem::path> model_path;  // the model's .gguf file
    std::string model_key;                            // "parakeet-v3"
    std::string model_label;                          // "Parakeet TDT 0.6B v3"
    std::string device;                               // "cpu", "cuda" (or "metal")
    std::string requested_device;                     // the setting: "auto", "cpu" or "cuda"
    int gpu_index = -1;                               // -1 on the CPU, else the first GPU
    // The runtime component ("asr-nvidia-cuda"); empty when the platform has none for the
    // device or this computer cannot run it.
    std::string runtime_id;
    // One of: "{Name}'s {DEVICE} runtime is not available on this platform.", "{Name} is not
    // available on this computer. {reason}.", "Install {Name}'s {GPU|CPU} runtime in
    // Downloads.", "Download {label} in Downloads." (Name is "Parakeet").
    std::optional<std::string> missing_message;
    EngineMissing missing_state = EngineMissing::None;

    bool usable() const { return runtime_dir && model_path && !missing_message; }
};

// Resolves the Parakeet engine from a settings snapshot: the model is local_asr_models'
// Parakeet-family choice, the device local_asr_devices' (Auto keeps the CPU runtime until the GPU
// one is installed; an explicit GPU never falls back). fv_speech_runtime, once chosen, fixes
// the runtime build ("cpu": the CPU runtime on the CPU; "cuda": the GPU runtime, on the CPU only
// when the device setting says so); fv_runtime_dir, when set, stands in for the runtime
// component (any build), so only the model is checked. Runtimes are looked for in
// paths.components_root(), models in paths.speech_models_root(). The model is checked even
// when the runtime is missing, so setup can show both requirements. A missing runtime still
// takes precedence in the load error.
ResolvedEngine resolve_parakeet(const Json& settings, const Paths& paths);
// The same with an explicit host (platform and GPU); host.is_installed is replaced by
// engine_runtime_installed.
ResolvedEngine resolve_parakeet(const Json& settings, const Paths& paths, Host host);

// Whether a NeMo-Speech.cpp folder the user chose (fv_runtime_dir) stands in for the runtime
// component `component_id`: it holds the runtime library, and for a GPU runtime it is a GPU
// build (engine runtime_has_gpu_backend). Reads the disk.
bool runtime_dir_serves(const std::filesystem::path& runtime_dir, std::string_view component_id);
// What the engine counts as an installed runtime: with fv_runtime_dir set, runtime_dir_serves
// for that folder (so Auto keeps the CPU with a CPU build); otherwise the component's sentinel
// under `components_root`.
std::function<bool(std::string_view)>
engine_runtime_installed(const Json& settings, std::filesystem::path components_root);

} // namespace fv::models
