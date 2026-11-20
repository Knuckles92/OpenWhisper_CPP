#pragma once

#include <span>
#include <string>
#include <string_view>

#include "base/json.h"

// The speech models and backends FioriVoce offers (services/local_asr/catalog.py MODELS,
// BACKENDS, DEFAULT_MODELS, artifacts, selected_model, selected_device) and the Parakeet entry of
// config.py's MODEL_CHOICES / MODEL_VALUE_MAP. Only the Parakeet family is kept: Parakeet TDT
// 0.6B v3 and Orukeet; Qwen3-ASR, Nemotron, Moonshine, Parakeet MLX and Parakeet Unified are not.
namespace fv::models {

struct SpeechModel {
    std::string_view key;
    std::string_view backend;
    std::string_view label;
    std::string_view purpose;
    std::string_view license;
    std::string_view languages;
    bool streaming = false;
    bool meeting = false;
    // Whether a finished native stream's text is as good as decoding the saved recording.
    bool stream_final = false;
};

struct SpeechBackend {
    std::string_view key;   // "parakeet"
    std::string_view label; // "Parakeet"
};

// The engine picker's entries: a label and the selected_model value it saves.
struct BackendChoice {
    std::string_view label;
    std::string_view value;
};

// The backend id catalog.py gives model names that are not in MODELS (Whisper's).
inline constexpr std::string_view kWhisperBackend = "local_whisper";

std::span<const SpeechModel> models();              // MODELS, in its order
const SpeechModel* model(std::string_view key);     // nullptr when not in MODELS
std::span<const SpeechBackend> backends();          // BACKENDS
const SpeechBackend* backend(std::string_view key); // nullptr when not in BACKENDS
// DEFAULT_MODELS: the first model of the backend. Throws std::out_of_range for another backend.
std::string_view default_model(std::string_view backend);
// The backend that owns a model name; kWhisperBackend for any name not in MODELS.
std::string_view backend_of(std::string_view model_name);
// The pinned artifacts of a model, {"repo", "revision", "files": [{"name", "url", "sha256",
// "size_bytes"}]}, as models.json has them. Throws std::out_of_range for a key without pins.
const Json& artifacts(std::string_view key);
// MODEL_CHOICES / MODEL_VALUE_MAP restricted to the backends this build has: Parakeet only.
std::span<const BackendChoice> backend_choices();

// The model local_asr_models saves for `backend` when it is one of that backend's models, else
// the backend's default: a saved choice never crosses model families.
// (catalog.py takes the backend first; the settings snapshot comes first here, as in
// resolve_parakeet.)
std::string selected_model(const Json& settings, std::string_view backend);
// The device local_asr_devices saves for `backend`: "auto", "cpu" or "cuda" ("auto" for
// anything else); Parakeet MLX accepts only "auto" and "cpu", Moonshine is always "cpu".
std::string selected_device(const Json& settings, std::string_view backend);

} // namespace fv::models
