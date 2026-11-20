#include "settings/model_selection.h"

#include <algorithm>
#include <array>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include "base/clock.h"
#include "base/log.h"
#include "pyport/text.h"
#include "settings/keys.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"services.settings"};

// keys::choices::selected_model is MODEL_VALUE_MAP's values less the dropped engines.
constexpr std::span<const std::string_view> kModelValues = keys::choices::selected_model;

// LEGACY_API_MODELS (services/settings.py:48-52).
constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kLegacyApiModels{{
    {"api_whisper", "whisper-1"},
    {"api_gpt4o", "gpt-4o-transcribe"},
    {"api_gpt4o_mini", "gpt-4o-mini-transcribe"},
}};

// config.API_MODEL_CHOICES and services/openai_retirement.py: OpenAI stops serving three of
// them on its shutdown date.
constexpr std::array<std::string_view, 4> kApiModelChoices{"gpt-transcribe", "gpt-4o-transcribe",
                                                           "gpt-4o-mini-transcribe", "whisper-1"};
constexpr std::array<std::string_view, 3> kRetiringModels{"gpt-4o-transcribe",
                                                          "gpt-4o-mini-transcribe", "whisper-1"};
constexpr std::chrono::year_month_day kShutdownDate{std::chrono::year{2027}, std::chrono::month{2},
                                                    std::chrono::day{26}};

bool contains(std::span<const std::string_view> values, std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

std::optional<std::string_view> legacy_api_model(const Json& selected) {
    if (!selected.is_string()) {
        return std::nullopt;
    }
    const auto& name = selected.get_ref<const std::string&>();
    for (const auto& [legacy, model] : kLegacyApiModels) {
        if (legacy == name) {
            return model;
        }
    }
    return std::nullopt;
}

// serving_api_model: `model` while OpenAI serves it, otherwise the default API model.
std::string serving_api_model(std::string_view model, std::chrono::year_month_day today) {
    const bool retired = today >= kShutdownDate;
    if (contains(kApiModelChoices, model) && !(retired && contains(kRetiringModels, model))) {
        return std::string(model);
    }
    return keys::api_transcription_model.default_value();
}

// resolve_api_transcription_model (services/settings.py:82-92).
std::string resolve_api_transcription_model(const Json& settings,
                                            std::chrono::year_month_day today) {
    const auto model = keys::api_transcription_model.stored(settings);
    if (model && contains(kApiModelChoices, *model)) {
        return serving_api_model(*model, today);
    }
    // The raw selection: the typed read already maps these names to "api".
    const auto found = settings.find(keys::selected_model.key());
    if (found != settings.end()) {
        if (const auto legacy = legacy_api_model(*found)) {
            return serving_api_model(*legacy, today);
        }
    }
    return keys::api_transcription_model.default_value();
}

std::chrono::year_month_day local_today() {
    const Clock& clock = system_clock();
    const auto local = clock.zone()->to_local(clock.utc());
    return std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(local)};
}

} // namespace

std::span<const std::string_view> model_values() {
    return kModelValues;
}

std::string load_model_selection(SettingsStore& store) {
    return load_model_selection(store, local_today());
}

std::string load_model_selection(SettingsStore& store, std::chrono::year_month_day today) {
    try {
        const auto settings = store.snapshot();
        const auto found = settings->find(keys::selected_model.key());
        if (found != settings->end()) {
            if (legacy_api_model(*found)) {
                Json patch = Json::object();
                patch[std::string(keys::selected_model.key())] = "api";
                patch[std::string(keys::api_transcription_model.key())] =
                    resolve_api_transcription_model(*settings, today);
                store.update(patch);
                return "api";
            }
            if (found->is_string() &&
                contains(kModelValues, found->get_ref<const std::string&>())) {
                return found->get<std::string>();
            }
        }
    } catch (const std::exception& error) {
        kLog.warning("Failed to load model selection: {}; using the default", error.what());
    }
    return keys::selected_model.default_value();
}

void save_model_selection(SettingsStore& store, std::string_view model) {
    if (model.empty()) {
        throw std::invalid_argument("model_value must be a non-empty string");
    }
    if (!contains(kModelValues, model)) {
        std::string valid = "[";
        for (const std::string_view value : kModelValues) {
            valid += (valid.size() > 1 ? ", " : "") + py::repr(value);
        }
        throw std::invalid_argument("Invalid model '" + std::string(model) +
                                    "'. Valid models: " + valid + "]");
    }
    try {
        store.save_setting(keys::selected_model.key(), Json(std::string(model)));
        kLog.info("Model selection saved: {}", model);
    } catch (const std::exception& error) {
        kLog.error("Failed to save model selection: {}", error.what());
        throw;
    }
}

} // namespace fv
