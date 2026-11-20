#include "llm/credentials.h"

#include "base/identity.h"

namespace fv::llm {

std::optional<std::string> lookup_env_value(std::string_view env_name, KeySources& keys) {
    if (env_name.empty()) {
        return std::nullopt;
    }
    return resolve_credential(keys.store, keys.env, keys.dotenv, env_name).value;
}

std::optional<std::string> resolve_api_key(const TextLLMProfile& profile, KeySources& keys) {
    if (profile.api_key_env.empty()) {
        return std::string(kAuthFreeApiKey);
    }
    return lookup_env_value(profile.api_key_env, keys);
}

std::string credential_label(const TextLLMProfile& profile) {
    if (profile.id == kOpenAiProfileId) {
        return "OpenAI API key";
    }
    if (profile.id == kOpenRouterProfileId) {
        return "OpenRouter API key";
    }
    if (profile.builtin) {
        return profile.api_key_env.empty() ? std::string() : profile.name + " API key";
    }
    return profile.api_key_env;
}

KeyStatus credential_status(const TextLLMProfile& profile, KeySources& keys) {
    if (profile.api_key_env.empty()) {
        return {true, "No API key required"};
    }
    const std::string label = credential_label(profile);
    const auto value = lookup_env_value(profile.api_key_env, keys);
    if (value && !value->empty()) {
        return {true, label + " found"};
    }
    return {false, "Requires " + label + " — add it in Settings → App → API keys"};
}

ConnectionFingerprint connection_fingerprint(const TextLLMProfile& profile, KeySources& keys) {
    return {profile.id, profile.kind, profile.base_url, profile.api_key_env,
            resolve_api_key(profile, keys)};
}

std::optional<http::Headers> provider_headers(const TextLLMProfile& profile,
                                              std::string_view session_id) {
    const std::string product(identity().product);
    if (profile.kind == kProfileKindOpenRouter) {
        return http::Headers{{"X-Title", product}};
    }
    if (profile.kind == "opencode_go" || profile.kind == "opencode_zen") {
        http::Headers headers{{"User-Agent", product}};
        if (profile.kind == "opencode_go" && !session_id.empty()) {
            headers.emplace_back("x-opencode-session", std::string(session_id));
        }
        return headers;
    }
    return std::nullopt;
}

} // namespace fv::llm
