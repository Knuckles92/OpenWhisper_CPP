#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

// services/text_llm.py's endpoint profiles: the ten built-in text-model endpoints and up to 50
// custom OpenAI-compatible ones (settings key text_llm_profiles), the local-server overrides
// "<id>_base_url" and "<id>_api_key_env", and the non-secret snapshots meetings keep.
// Every function is pure over its arguments and thread-safe. Readers take a settings snapshot;
// mutators edit the mapping they are given and throw std::invalid_argument with the user's
// sentence.
namespace fv::llm {

inline constexpr std::string_view kProfileKindOpenAi = "openai";
inline constexpr std::string_view kProfileKindOpenRouter = "openrouter";
inline constexpr std::string_view kProfileKindCustom = "custom";
inline constexpr std::string_view kOpenAiProfileId = "openai";
inline constexpr std::string_view kOpenRouterProfileId = "openrouter";

// OpenAI's client requires a non-empty key even when the server ignores it.
inline constexpr std::string_view kAuthFreeApiKey = "dummy";
inline constexpr std::size_t kMaxProfileNameLen = 80; // code points
inline constexpr std::size_t kMaxCustomProfiles = 50;
inline constexpr std::string_view kOpenRouterBaseUrl = "https://openrouter.ai/api/v1";

// A built-in local inference server: one slot each, at the server's standard local address.
struct LocalServer {
    std::string_view id;
    std::string_view default_url;
    std::string_view name;
};

// LOCAL_SERVER_DEFAULT_URLS and LOCAL_SERVER_NAMES, in their order.
inline constexpr std::array<LocalServer, 5> kLocalServers{{
    {"ollama", "http://localhost:11434/v1", "Ollama"},
    {"lm_studio", "http://127.0.0.1:1234/v1", "LM Studio"},
    {"vllm", "http://127.0.0.1:8000/v1", "vLLM"},
    // llama-server moved from 8080 to 9931 in October 2026, leaving 8080 to LocalAI; an older
    // llama.cpp is found there by server detection.
    {"llama_cpp", "http://127.0.0.1:9931/v1", "llama.cpp"},
    {"localai", "http://127.0.0.1:8080/v1", "LocalAI"},
}};

// Hosted providers whose request shape comes from a reviewed catalog.
inline constexpr std::array<std::string_view, 3> kHostedCatalogProfileIds{"groq", "opencode_go",
                                                                          "opencode_zen"};

// BUILTIN_KINDS (= BUILTIN_PROFILE_IDS: every built-in profile's id is its kind), in order.
inline constexpr std::array<std::string_view, 10> kBuiltinKinds{
    "openai",    "openrouter", "ollama", "lm_studio",   "vllm",
    "llama_cpp", "localai",    "groq",   "opencode_go", "opencode_zen"};

bool is_local_server_id(std::string_view id);
std::optional<std::string_view> local_server_default_url(std::string_view id);
bool is_builtin_kind(std::string_view kind);
// PROFILE_KINDS: the built-in kinds and "custom".
bool is_profile_kind(std::string_view kind);

// True when `url`'s host is this computer (127.0.0.1, localhost, ::1). Throws
// std::invalid_argument for a URL urlsplit refuses ("Invalid IPv6 URL").
bool is_loopback_url(std::optional<std::string_view> url);

// A named chat endpoint; its credential stays outside the settings.
struct TextLLMProfile {
    std::string id;
    std::string name;
    std::string kind;
    std::optional<std::string> base_url;
    std::string api_key_env;
    bool builtin = false;
    std::optional<Json> model_metadata; // a JSON object
    std::string protocol = "chat";

    bool requires_api_key() const { return !api_key_env.empty(); }
    // The endpoint host is loopback.
    bool is_local() const;
    // A server the user runs: a local server preset wherever it lives, or any endpoint here.
    bool self_hosted() const;
    // The API itself refuses tools a model cannot call (OpenAI, and OpenRouter, which routes a
    // tool request only to providers that support tools).
    bool refuses_unsupported_tools() const;

    bool operator==(const TextLLMProfile& other) const;
};

// The non-secret connection snapshot persisted with a meeting.
struct TextLLMSnapshot {
    std::string profile_id;
    std::string name;
    std::string kind;
    std::optional<std::string> base_url;
    std::string api_key_env;
    std::optional<Json> model_metadata;
    std::string protocol = "chat";

    // model_metadata only when non-empty, protocol only when not "chat".
    Json to_dict() const;
    TextLLMProfile to_profile() const;
};

// The built-in defaults; configured local-server URLs resolve in get_profile.
const std::vector<TextLLMProfile>& builtin_profiles();
std::optional<TextLLMProfile> builtin_profile(std::string_view profile_id);

// A canonical http(s) API URL: stripped, trailing "/" removed, fragment dropped, query kept.
// Throws std::invalid_argument: "Base URL is required", "Base URL must use http or https",
// "Base URL is missing a host".
std::string normalize_base_url(std::string_view url);
// Stripped; "Endpoint name is required", "Endpoint name must be 80 characters or fewer".
std::string validate_profile_name(std::string_view name);
// Stripped, possibly empty; else an uppercase identifier, or "API key variable must be empty or
// an uppercase identifier (A–Z, digits, underscore)".
std::string validate_api_key_env(std::string_view name);
// "custom_" and eight random hex digits.
std::string new_custom_profile_id();
// "chat" or "responses", else "Choose Chat Completions or Responses for this endpoint.".
std::string validate_text_protocol(const Json& protocol);

// Freezes `profile`'s connection. With `model`, the snapshot records model_spec(profile, model)
// as model_metadata from what is known now; OpenWhisper first asks a server the user runs about
// its models, which the overload in llm/local_servers.h does (learn_server_models). Throws what
// model_spec throws.
TextLLMSnapshot snapshot_from_profile(const TextLLMProfile& profile, std::string_view model = {});

// A snapshot from a saved mapping or its JSON text (a blank or unreadable text, or anything but
// an object, is nullopt). nullopt as well without a profile id, for a base URL
// normalize_base_url refuses, or without a URL for anything but OpenAI. Throws
// std::invalid_argument for an unknown protocol or invalid model metadata ("Invalid saved text
// model metadata"), and what spec_from_mapping throws.
std::optional<TextLLMSnapshot> snapshot_from_mapping(const Json& raw);

// A custom profile from one saved entry; nullopt unless the id starts with "custom_" and the
// name, URL, key variable and protocol are valid.
std::optional<TextLLMProfile> parse_custom_profile(const Json& raw);
// The valid custom profiles in saved order, without repeated ids, at most 50.
std::vector<TextLLMProfile> list_custom_profiles(const Json& settings);
// The built-in profiles (local servers configured) followed by the custom ones.
std::vector<TextLLMProfile> list_profiles(const Json& settings);
// A local server's API URL: credentials and a query are refused ("Use a server URL without
// credentials or query parameters."), and a bare host gets "/v1".
std::string normalize_local_server_url(std::string_view url);
// Saves one local server preset's URL and key variable into `settings` (Python saves them with
// update_settings; the caller runs this inside SettingsStore::mutate). Throws
// std::invalid_argument("'<id>' is not a built-in local server.") and the validators' errors,
// leaving `settings` unchanged.
void save_local_server(Json& settings, std::string_view profile_id, std::string_view url,
                       std::string_view api_key_env = {});
// A built-in or custom profile by id (stripped); nullopt for a blank or unknown id. A local
// server's saved URL that does not parse falls back to its default silently.
std::optional<TextLLMProfile> get_profile(std::optional<std::string_view> profile_id,
                                          const Json& settings);
std::vector<std::string> known_profile_ids(const Json& settings);
// The saved form of the custom profiles among `profiles`, at most 50.
Json custom_profiles_payload(const std::vector<TextLLMProfile>& profiles);

// Creates (no `profile_id`) or replaces a validated custom endpoint in `settings` and returns
// it. An edit keeps the existing protocol when none is given. Throws std::invalid_argument:
// the validators' errors, "Cannot edit a built-in endpoint", "At most 50 custom endpoints can be
// saved".
TextLLMProfile upsert_custom_profile(Json& settings, std::string_view name,
                                     std::string_view base_url, std::string_view api_key_env = {},
                                     std::optional<std::string_view> profile_id = std::nullopt,
                                     std::optional<std::string_view> protocol = std::nullopt);
// Deletes a custom profile; false when there was none. Throws std::invalid_argument("Built-in
// endpoints cannot be deleted").
bool remove_custom_profile(Json& settings, std::string_view profile_id);

// The name shown for a profile id: an installed agent's name, the profile's, else the id.
std::string profile_display_name(std::optional<std::string_view> profile_id, const Json& settings);
// OpenRouter's and OpenAI's cleanup defaults; "" for any other endpoint.
std::string default_model_for_profile(const TextLLMProfile& profile);

// Per-request options of a cleanup-style chat call: {"temperature": 0}, or OpenRouter's
// {"extra_body": {"reasoning": {"effort": level}}}, or OpenAI's {"reasoning_effort": level}.
Json chat_request_options(const TextLLMProfile& profile, std::string_view reasoning = "off");
// "your local server at <host>", "OpenRouter (openrouter.ai)", "OpenAI (api.openai.com)", or
// "<name> (<host>)".
std::string consent_destination(const TextLLMProfile& profile);
bool destination_is_remote(const TextLLMProfile& profile);

// The profile `provider` names, else the meeting endpoint chosen now (never a fixed cloud
// provider: a meeting recorded on a deleted local server must not go elsewhere).
TextLLMProfile profile_or_meeting_default(std::optional<std::string_view> provider,
                                          const Json& settings);
// A meeting row's endpoint: its agent_endpoint_json snapshot, else agent_provider's profile
// (fallback_provider for rows that predate it). Throws what snapshot_from_mapping throws.
TextLLMSnapshot snapshot_from_meeting(const Json& meeting, const Json& settings,
                                      std::string_view fallback_provider = kOpenRouterProfileId);
// The live profile of a meeting agent, preferring its persisted endpoint snapshot.
TextLLMProfile profile_from_agent_config(std::string_view provider, const Json& endpoint,
                                         const Json& settings);

} // namespace fv::llm
