#include "models/meeting_speech.h"

#include <exception>
#include <format>
#include <utility>

#include "base/log.h"
#include "models/speech_catalog.h"
#include "settings/keys.h"

namespace fv::models {

namespace {

constexpr log::Logger kLog("services.settings");

constexpr std::string_view kParakeet = "parakeet";
constexpr std::string_view kLocal = "local";
constexpr std::string_view kRemote = "remote";
constexpr std::string_view kOpenAiSpeakers = "openai";

// The platform's default meeting model (config.MEETING_ASR_MODEL on a Parakeet platform).
std::string default_meeting_model() {
    return std::string(default_model(kParakeet));
}

// _own_meeting_model: the model saved for meetings alone, then a compatible Parakeet default.
std::string own_meeting_model(const Json& settings) {
    // Only a saved choice counts here; with none, the defaults come last.
    if (const auto extra = keys::meeting_asr_model.stored(settings)) {
        if (const SpeechModel* entry = model(*extra); entry != nullptr && entry->meeting) {
            return *extra;
        }
    }
    // The stored value, not the validated one: a malformed selection has never counted as
    // Parakeet here.
    const std::string_view key = keys::selected_model.key();
    const auto saved = settings.is_object() ? settings.find(key) : settings.end();
    const Json engine =
        saved != settings.end() ? *saved : Json(keys::selected_model.default_value());
    if (engine.is_string() && engine.get_ref<const std::string&>() == kParakeet) {
        return selected_model(settings, kParakeet);
    }
    return default_meeting_model();
}

} // namespace

std::string meeting_model_label(std::string_view model_name) {
    if (const SpeechModel* entry = model(model_name)) {
        return std::string(entry->label);
    }
    return std::string(model_name);
}

MeetingSpeech resolve_meeting_speech(const Json& settings) {
    const std::string own = own_meeting_model(settings);
    if (keys::meeting_speech_separate.read(settings)) {
        const bool remote = keys::meeting_asr_source.read(settings) == kRemote;
        return MeetingSpeech{std::string(remote ? kRemote : kLocal), own, true, {}};
    }
    // A pre-2.0 API model name reads as the API backend.
    const std::string engine = keys::selected_model.read(settings);
    if (engine == kRemote) {
        return MeetingSpeech{std::string(kLocal), own, false,
                             std::format("Meeting speech stays on this computer and uses {}.",
                                         meeting_model_label(own))};
    }
    std::string name;
    if (backend(engine) != nullptr) {
        const std::string chosen = selected_model(settings, engine);
        const SpeechModel* entry = model(chosen);
        if (entry != nullptr && entry->meeting) {
            return MeetingSpeech{std::string(kLocal), chosen, false, {}};
        }
        name = entry != nullptr ? std::string(entry->label) : chosen;
    } else {
        name = "The API engine";
    }
    const std::string fallback = default_meeting_model();
    return MeetingSpeech{std::string(kLocal), fallback, false,
                         std::format("{} can't transcribe meetings, so meetings use {}.", name,
                                     meeting_model_label(fallback))};
}

MeetingSpeech own_meeting_speech(const Json& settings) {
    Json separate = settings.is_object() ? settings : Json::object();
    separate[std::string(keys::meeting_speech_separate.key())] = true;
    return resolve_meeting_speech(separate);
}

std::string resolve_meeting_asr_source(const Json& settings) {
    return resolve_meeting_speech(settings).source;
}

std::string resolve_meeting_whisper_model(const Json& settings) {
    return resolve_meeting_speech(settings).model;
}

std::string meeting_model_in_use(const Json& settings) {
    MeetingSpeech speech = resolve_meeting_speech(settings);
    return speech.source == kRemote ? std::string() : std::move(speech.model);
}

bool keep_separate_meeting_speech(SettingsStore& store) {
    const bool is_new = store.is_new_install();
    const std::string key(keys::meeting_speech_separate.key());
    if (store.snapshot()->contains(key)) {
        return false;
    }
    bool kept = false;
    try {
        store.mutate([&](Json& settings) {
            if (settings.contains(key)) {
                kept = false;
                return;
            }
            const MeetingSpeech own = own_meeting_speech(settings);
            // Keep legacy engine preferences explicit without inheriting a remote route.
            const auto follows = keys::selected_model.read(settings) == kRemote
                                     ? MeetingSpeech{std::string(kRemote), own.model, false, {}}
                                     : resolve_meeting_speech(settings);
            const bool same = own.source == follows.source &&
                              (own.source == kRemote || own.model == follows.model);
            settings[key] = !(is_new || same);
            kept = true;
        });
        return kept;
    } catch (const std::exception& error) {
        kLog.warning("Could not save the install's meeting engine: {}", error.what());
        return false;
    }
}

std::string resolve_meeting_speaker_id_backend(const Json& settings, const Clock& /*clock*/) {
    std::string backend_name = keys::meeting_speaker_id_backend.read(settings);
    if (backend_name == kOpenAiSpeakers) {
        return std::string(kLocal);
    }
    return backend_name;
}

} // namespace fv::models
