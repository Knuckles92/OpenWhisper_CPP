#include "models/speech_catalog.h"

#include <array>
#include <stdexcept>

#include "models/data/speech_catalog.json.h"
#include "settings/keys.h"

namespace fv::models {

namespace {

// key, backend, label, purpose, license, languages, streaming, meeting, stream_final.
constexpr std::array<SpeechModel, 2> kModels{{
    {"parakeet-v3", "parakeet", "Parakeet TDT 0.6B v3", "Fast dictation and files", "CC-BY-4.0",
     "25 European languages", false, true, false},
    {"orukeet-v0.1", "parakeet", "Orukeet TDT 0.6B",
     "Community adaptation of Parakeet for multilingual transcription", "CC-BY-SA-4.0",
     "25 European languages, including Russian and English", false, true, false},
}};

constexpr std::array<SpeechBackend, 1> kBackends{{{"parakeet", "Parakeet"}}};

constexpr std::array<BackendChoice, 1> kChoices{{{"Parakeet", "parakeet"}}};

const Json& pinned_models() {
    static const Json pins = Json::parse(embedded::models_speech_catalog).at("models");
    return pins;
}

// The value saved under `backend` in a dict setting, or nullptr.
const Json* saved_for(const Json& mapping, std::string_view backend) {
    if (!mapping.is_object()) {
        return nullptr;
    }
    const auto it = mapping.find(backend);
    return it == mapping.end() ? nullptr : &*it;
}

} // namespace

std::span<const SpeechModel> models() {
    return kModels;
}

const SpeechModel* model(std::string_view key) {
    for (const auto& candidate : kModels) {
        if (candidate.key == key) {
            return &candidate;
        }
    }
    return nullptr;
}

std::span<const SpeechBackend> backends() {
    return kBackends;
}

const SpeechBackend* backend(std::string_view key) {
    for (const auto& candidate : kBackends) {
        if (candidate.key == key) {
            return &candidate;
        }
    }
    return nullptr;
}

std::string_view default_model(std::string_view backend_key) {
    if (backend(backend_key) != nullptr) {
        for (const auto& candidate : kModels) {
            if (candidate.backend == backend_key) {
                return candidate.key;
            }
        }
    }
    throw std::out_of_range("no default model for backend " + std::string(backend_key));
}

std::string_view backend_of(std::string_view model_name) {
    const SpeechModel* found = model(model_name);
    return found != nullptr ? found->backend : kWhisperBackend;
}

const Json& artifacts(std::string_view key) {
    const Json& pins = pinned_models();
    const auto it = pins.find(key);
    if (it == pins.end()) {
        throw std::out_of_range("no pinned artifacts for " + std::string(key));
    }
    return *it;
}

std::span<const BackendChoice> backend_choices() {
    return kChoices;
}

std::string selected_model(const Json& settings, std::string_view backend_key) {
    const Json saved = keys::local_asr_models.read(settings);
    if (const Json* key = saved_for(saved, backend_key); key != nullptr && key->is_string()) {
        const SpeechModel* found = model(key->get_ref<const std::string&>());
        if (found != nullptr && found->backend == backend_key) {
            return std::string(found->key);
        }
    }
    return std::string(default_model(backend_key));
}

std::string selected_device(const Json& settings, std::string_view backend_key) {
    const Json saved = keys::local_asr_devices.read(settings);
    const Json* device = saved_for(saved, backend_key);
    const std::string value =
        device != nullptr && device->is_string() ? device->get<std::string>() : std::string();
    if (backend_key == "parakeet_mlx") {
        return value == "auto" || value == "cpu" ? value : "auto";
    }
    if (backend_key == "moonshine") {
        return "cpu";
    }
    return value == "auto" || value == "cpu" || value == "cuda" ? value : "auto";
}

} // namespace fv::models
