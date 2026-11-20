#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "llm/credentials.h"
#include "llm/profiles.h"

namespace fv::llm::detail {

// create_openai_client's `api_key or resolve_api_key(profile)`: the given key, "dummy" for an
// endpoint that needs none, else the resolved credential (nullopt without `keys`).
inline std::optional<std::string> key_for(const TextLLMProfile& profile, std::string_view key,
                                          KeySources* keys) {
    if (!key.empty()) {
        return std::string(key);
    }
    if (profile.api_key_env.empty()) {
        return std::string(kAuthFreeApiKey);
    }
    if (keys != nullptr) {
        return resolve_api_key(profile, *keys);
    }
    return std::nullopt;
}

} // namespace fv::llm::detail
