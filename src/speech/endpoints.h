#pragma once

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/cancel.h"
#include "base/env.h"
#include "base/json.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "http/client.h"

namespace fv::speech {

// Speech-to-text servers the api backend uploads audio to (services/speech_endpoints.py). Every
// one speaks OpenAI's POST /audio/transcriptions. OpenAI stays the default and keeps its own
// model list, hint fields and retirement dates; the rest are presets for servers with a standard
// address (each editable) plus one Custom slot for anything else.
//
// Settings: api_transcription_endpoint names the selected endpoint, api_transcription_servers
// holds each configurable endpoint's overrides (base_url, api_key_env, model). OpenAI's model
// stays under api_transcription_model.

inline constexpr std::string_view kOpenAiEndpointId = "openai";
inline constexpr std::string_view kCustomEndpointId = "custom";
// The SDK's address when an endpoint has no base URL.
inline constexpr std::string_view kOpenAiBaseUrl = "https://api.openai.com/v1";
// What is sent as the key to a server that needs none (text_llm.AUTH_FREE_API_KEY).
inline constexpr std::string_view kAuthFreeApiKey = "dummy";

// Read timeouts, seconds: one upload to a hosted service, and to a server someone runs
// themselves (it loads the model on the first request and may run on a CPU, where one upload
// chunk of several minutes of audio takes minutes; Cancel still stops it at once).
inline constexpr double kHostedTimeoutS = 120.0;
inline constexpr double kSelfHostedTimeoutS = 600.0;

// One transcription server and how to talk to it: the connection half is text_llm's
// TextLLMProfile (id, name, kind, base_url, api_key_env, builtin).
struct SpeechEndpoint {
    std::string id;
    std::string name;
    std::string kind;
    std::optional<std::string> base_url; // nullopt: OpenAI's own address
    std::string api_key_env;             // "": the server needs no key
    bool builtin = true;
    // Model used until the user picks one.
    std::string default_model;
    // Whether the user may change the address, key variable and model id.
    bool configurable = false;
    std::string description;
    // Catalog route under the base URL; nullopt when the server has none, or when known_models
    // already says what it serves.
    std::optional<std::string> models_path = "/models";
    std::vector<std::string> known_models;
    // `language` value that asks for detection, for a server that assumes one language when the
    // field is absent.
    std::string auto_language;

    bool is_openai() const;
    // True when audio stays on this computer (a loopback address).
    bool is_local() const;
    bool is_configured() const;
    // A server the user runs: a server preset wherever it lives, or any endpoint on this
    // computer. The Custom slot is as likely a hosted gateway, so only its address decides.
    bool self_hosted() const;
    double timeout_s() const;

    bool operator==(const SpeechEndpoint&) const = default;
};

// Display order: OpenAI, Groq, Speaches, LocalAI, vLLM, whisper.cpp, Custom server.
std::span<const SpeechEndpoint> speech_endpoints();
std::vector<std::string> endpoint_ids();

// `endpoint_id`'s preset with the user's address and key variable. An unknown id is OpenAI's, so
// a damaged setting never sends audio somewhere the user did not choose; an invalid saved
// server is ignored (logged) and the preset returned.
SpeechEndpoint configured_endpoint(std::string_view endpoint_id, const Json& settings);
// The endpoint the api backend uploads to; a Custom slot without an address reads as OpenAI.
SpeechEndpoint selected_endpoint(const Json& settings);
// The model `endpoint` transcribes with: OpenAI's through its retirement rules on `today`, any
// other server's the user's own model id, or the preset's default.
std::string endpoint_model(const SpeechEndpoint& endpoint, const Json& settings,
                           std::chrono::year_month_day today);
std::string selected_model(const Json& settings, std::chrono::year_month_day today);

// text_llm.normalize_base_url: a canonical http(s) URL without a trailing slash or fragment.
// Throws std::invalid_argument ("Base URL is required", "Base URL must use http or https",
// "Base URL is missing a host").
std::string normalize_base_url(std::string_view url);
// A canonical server URL, "/v1" added when it has no path. Throws std::invalid_argument, the
// message fit to show.
std::string valid_server_url(std::string_view url);
// A stripped environment-variable name, possibly empty. Throws std::invalid_argument.
std::string validate_api_key_env(std::string_view name);
// True when `url`'s host is this computer (127.0.0.1, localhost or ::1).
bool is_loopback_url(const std::optional<std::string>& url);

// The mutators edit the settings mapping (SettingsStore::mutate) and throw std::invalid_argument
// with the text to show for a value that will not do, leaving it unchanged.
void save_selection(Json& settings, std::string_view endpoint_id);
// Remembers `endpoint_id`'s model; OpenAI's keeps its own setting.
void save_model(Json& settings, std::string_view endpoint_id, std::string_view model);
// Persists a configurable server's address, key variable and model.
void save_server(Json& settings, std::string_view endpoint_id, std::string_view base_url,
                 std::string_view api_key_env, std::string_view model);

// The key to send: kAuthFreeApiKey for a server without one, the resolved credential (saved
// key, environment, .env), or nullopt when it is missing. Blocking (the credential store); never
// on the GUI thread.
std::optional<std::string> api_key(const SpeechEndpoint& endpoint, CredentialStore& credentials,
                                   const EnvReader& env, const DotEnv& dotenv);
// Where uploaded audio goes, for privacy copy: "OpenAI (api.openai.com)", "Groq
// (api.groq.com)", "your local server at 127.0.0.1:8000".
std::string destination(const SpeechEndpoint& endpoint);
// One sentence saying where dictated audio goes.
std::string audio_destination_sentence(const SpeechEndpoint& endpoint);
// (variable, endpoint name) for every usable endpoint that sends a key, in display order.
std::vector<std::pair<std::string, std::string>> credential_owners(const Json& settings);

// Whether a catalog entry is a speech model: Speaches tags each entry with a task, LocalAI's
// capabilities catalog lists what each model can do.
bool is_speech_model(const Json& item);

// Speech model ids `endpoint` offers. OpenAI's are the curated list for `today`; a server
// without a catalog route its known models (none: the user's own id is the only choice); any
// other is asked GET {base}{models_path} with `key` as a bearer token (when set), 5 s timeouts,
// and the ids sorted (a 404 lists none). Throws std::runtime_error when the request fails, so the
// caller can say so. Blocking; never on the GUI thread.
std::vector<std::string> list_models(http::Client& client, const SpeechEndpoint& endpoint,
                                     const std::optional<std::string>& key,
                                     const CancelToken& cancel, std::chrono::year_month_day today);
// As above, `today` by the system clock's local date.
std::vector<std::string> list_models(http::Client& client, const SpeechEndpoint& endpoint,
                                     const std::optional<std::string>& key,
                                     const CancelToken& cancel);

} // namespace fv::speech
