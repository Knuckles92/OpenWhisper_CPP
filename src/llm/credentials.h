#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "base/env.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "http/client.h"
#include "llm/profiles.h"

// services/text_llm.py's credential half (612-672): which key a text-model endpoint needs,
// where it is found, and the provider headers. Every lookup reads the credential store, so
// these block and never run on the GUI thread. No key reaches a log.
namespace fv::llm {

// Where keys are looked up: the saved key, then the process environment, then .env.
struct KeySources {
    CredentialStore& store;
    const EnvReader& env;
    const DotEnv& dotenv;
};

// One credential by its variable name; nullopt for an empty name or when nothing is found.
std::optional<std::string> lookup_env_value(std::string_view env_name, KeySources& keys);
// The key to send: "dummy" for an endpoint that needs none, else the resolved credential, or
// nullopt when a required key is missing.
std::optional<std::string> resolve_api_key(const TextLLMProfile& profile, KeySources& keys);
// "OpenAI API key", "OpenRouter API key", "<name> API key" for another built-in that needs one
// ("" when it needs none), else the custom endpoint's variable name.
std::string credential_label(const TextLLMProfile& profile);

struct KeyStatus {
    bool available = false;
    std::string text; // "No API key required", "<label> found", or where to add it
    bool operator==(const KeyStatus&) const = default;
};
KeyStatus credential_status(const TextLLMProfile& profile, KeySources& keys);

// What identifies a live connection: a change in any part means a new client.
struct ConnectionFingerprint {
    std::string id;
    std::string kind;
    std::optional<std::string> base_url;
    std::string api_key_env;
    std::optional<std::string> key; // never logged
    bool operator==(const ConnectionFingerprint&) const = default;
};
ConnectionFingerprint connection_fingerprint(const TextLLMProfile& profile, KeySources& keys);

// Headers every request to the endpoint carries: OpenRouter's X-Title and OpenCode's
// User-Agent (both identity().product), plus OpenCode Go's x-opencode-session when a session is
// given; nullopt for every other endpoint.
std::optional<http::Headers> provider_headers(const TextLLMProfile& profile,
                                              std::string_view session_id = {});

} // namespace fv::llm
