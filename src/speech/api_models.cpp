#include "speech/api_models.h"

#include <algorithm>
#include <utility>

#include "settings/keys.h"

namespace fv::speech {

namespace {

// services/settings.py LEGACY_API_MODELS.
constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kLegacyApiModels{{
    {"api_whisper", "whisper-1"},
    {"api_gpt4o", "gpt-4o-transcribe"},
    {"api_gpt4o_mini", "gpt-4o-mini-transcribe"},
}};

bool is_choice(std::string_view model) {
    return std::ranges::find(kApiModelChoices, model) != kApiModelChoices.end();
}

} // namespace

std::chrono::year_month_day local_today(const Clock& clock) {
    const auto local = clock.zone()->to_local(clock.utc());
    return std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(local)};
}

bool retired(std::chrono::year_month_day today) {
    return today >= kShutdownDay;
}

bool is_retiring_model(std::string_view model) {
    return std::ranges::find(kRetiringTranscriptionModels, model) !=
           kRetiringTranscriptionModels.end();
}

std::vector<std::string> api_model_choices(std::chrono::year_month_day today) {
    std::vector<std::string> choices;
    for (const std::string_view model : kApiModelChoices) {
        if (!retired(today) || !is_retiring_model(model)) {
            choices.emplace_back(model);
        }
    }
    return choices;
}

std::string api_model_label(std::string_view model, std::chrono::year_month_day today) {
    if (is_retiring_model(model) && !retired(today)) {
        return std::string(model) + " (retiring " + std::string(kShutdownLabel) + ")";
    }
    return std::string(model);
}

std::string serving_api_model(std::string_view model, std::chrono::year_month_day today) {
    const auto choices = api_model_choices(today);
    if (std::ranges::find(choices, model) != choices.end()) {
        return std::string(model);
    }
    return std::string(kDefaultApiModel);
}

std::string resolve_api_transcription_model(const Json& settings,
                                            std::chrono::year_month_day today) {
    const auto model = keys::api_transcription_model.stored(settings);
    if (model && is_choice(*model)) {
        return serving_api_model(*model, today);
    }
    // The raw selection: the typed read already maps these names to "api".
    if (settings.is_object()) {
        const auto found = settings.find(keys::selected_model.key());
        if (found != settings.end() && found->is_string()) {
            const auto& legacy = found->get_ref<const std::string&>();
            for (const auto& [name, meant] : kLegacyApiModels) {
                if (name == legacy) {
                    return serving_api_model(meant, today);
                }
            }
        }
    }
    return keys::api_transcription_model.default_value();
}

bool is_model_gone_error(const std::exception& error) {
    const auto* failed = dynamic_cast<const SpeechApiError*>(&error);
    if (failed == nullptr) {
        return false;
    }
    return failed->status == 404 || failed->code == "model_not_found";
}

} // namespace fv::speech
