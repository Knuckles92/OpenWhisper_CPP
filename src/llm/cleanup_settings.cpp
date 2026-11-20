#include "llm/cleanup_settings.h"

#include <algorithm>
#include <utility>

#include "pyport/text.h"
#include "settings/keys.h"

namespace fv::llm {

namespace {

// _resolve_text_llm_assignment: the saved pair when the provider is still a known profile and
// the model is not blank, else both defaults. Only the cleanup pair keeps an installed agent.
std::pair<std::string, std::string> resolve_assignment(const StrSetting& provider_key,
                                                       const StrSetting& model_key, bool cleanup,
                                                       const Json& settings) {
    const auto provider = provider_key.stored(settings);
    const auto model = model_key.stored(settings);
    if (cleanup && provider && is_installed_cleanup_agent(*provider) && model) {
        return {*provider, py::strip(*model)};
    }
    if (provider && model) {
        const auto known = known_text_llm_profile_ids(settings);
        std::string stripped = py::strip(*model);
        if (std::ranges::find(known, *provider) != known.end() && !stripped.empty()) {
            return {*provider, std::move(stripped)};
        }
    }
    return {provider_key.default_value(), model_key.default_value()};
}

std::pair<std::string, std::string> cleanup_assignment(const Json& settings) {
    return resolve_assignment(keys::transcript_cleanup_provider, keys::transcript_cleanup_model,
                              true, settings);
}

std::pair<std::string, std::string> meeting_assignment(const Json& settings) {
    return resolve_assignment(keys::meeting_llm_provider, keys::meeting_llm_model, false, settings);
}

} // namespace

bool is_installed_cleanup_agent(std::string_view provider) {
    return std::ranges::find(kInstalledCleanupAgents, provider) != kInstalledCleanupAgents.end();
}

bool cleanup_agent_authorized(const Json& settings, std::string_view provider) {
    return settings.is_object() && is_installed_cleanup_agent(provider) &&
           keys::transcript_cleanup_enabled.read(settings) &&
           keys::fv_cleanup_agent_consent.read(settings) == provider &&
           resolve_transcript_cleanup_provider(settings) == provider;
}

std::vector<std::string> known_text_llm_profile_ids(const Json& settings) {
    return known_profile_ids(settings);
}

std::string default_transcript_cleanup_model(std::string_view provider, const Json& settings) {
    if (is_installed_cleanup_agent(provider)) {
        return {};
    }
    if (const auto profile = get_profile(provider, settings)) {
        return default_model_for_profile(*profile);
    }
    if (provider == "openrouter") {
        return std::string(kTranscriptCleanupOpenRouterModel);
    }
    return std::string(kTranscriptCleanupModel);
}

std::string resolve_transcript_cleanup_provider(const Json& settings) {
    return cleanup_assignment(settings).first;
}

std::string resolve_transcript_cleanup_model(const Json& settings) {
    auto [provider, model] = cleanup_assignment(settings);
    if (provider == "openai" && model == "gpt-4o-mini") {
        return std::string(kTranscriptCleanupModel);
    }
    return model;
}

std::string resolve_transcript_cleanup_reasoning(const Json& settings) {
    return keys::transcript_cleanup_reasoning.read(settings);
}

std::string resolve_transcript_cleanup_model_sort(const Json& settings) {
    return keys::transcript_cleanup_model_sort.read(settings);
}

std::string resolve_meeting_llm_provider(const Json& settings) {
    return meeting_assignment(settings).first;
}

TextLLMProfile resolve_meeting_llm_profile(const Json& settings) {
    const std::string profile_id = resolve_meeting_llm_provider(settings);
    if (auto profile = get_profile(profile_id, settings)) {
        return std::move(*profile);
    }
    if (auto profile = builtin_profile(profile_id)) {
        return std::move(*profile);
    }
    // Unreachable: the resolver returns a known id or "openrouter".
    return *builtin_profile(kOpenRouterProfileId);
}

std::string resolve_meeting_llm_model(const Json& settings) {
    return meeting_assignment(settings).second;
}

Json resolve_meeting_llm_endpoint(const Json& settings) {
    return snapshot_from_profile(resolve_meeting_llm_profile(settings),
                                 resolve_meeting_llm_model(settings))
        .to_dict();
}

} // namespace fv::llm
