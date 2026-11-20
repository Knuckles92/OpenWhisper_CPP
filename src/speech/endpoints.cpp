#include "speech/endpoints.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

#include "base/log.h"
#include "credentials/resolve.h"
#include "http/client.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "speech/api_models.h"
#include "speech/detail/url.h"

namespace fv::speech {

namespace {

constexpr log::Logger kLog{"services.speech_endpoints"};

constexpr std::string_view kSpeechTask = "automatic-speech-recognition";
constexpr std::string_view kSpeechCapability = "transcript";
// text_llm._LOCAL_HOSTS.
constexpr std::array<std::string_view, 3> kLocalHosts{"127.0.0.1", "localhost", "::1"};
// list_models' httpx timeout, seconds.
constexpr auto kCatalogTimeout = std::chrono::milliseconds(5'000);

SpeechEndpoint make_endpoint(std::string id, std::string name, std::optional<std::string> base_url,
                             std::string api_key_env, std::string default_model, bool hosted,
                             std::string description) {
    SpeechEndpoint endpoint;
    endpoint.kind = id == kCustomEndpointId ? "custom" : id;
    endpoint.builtin = id != kCustomEndpointId;
    endpoint.id = std::move(id);
    endpoint.name = std::move(name);
    endpoint.base_url = std::move(base_url);
    endpoint.api_key_env = std::move(api_key_env);
    endpoint.default_model = std::move(default_model);
    endpoint.configurable = !hosted;
    endpoint.description = std::move(description);
    return endpoint;
}

std::vector<SpeechEndpoint> build_endpoints() {
    std::vector<SpeechEndpoint> all;
    all.push_back(make_endpoint(std::string(kOpenAiEndpointId), "OpenAI", std::nullopt,
                                "OPENAI_API_KEY", std::string(kDefaultApiModel), true,
                                "OpenAI's transcription models. Needs an OpenAI API key."));
    // Groq's catalog mixes in its chat models without marking either.
    auto groq = make_endpoint("groq", "Groq", "https://api.groq.com/openai/v1", "GROQ_API_KEY",
                              "whisper-large-v3-turbo", true,
                              "Open-weights Whisper models hosted by Groq. Needs a Groq API key.");
    groq.models_path.reset();
    groq.known_models = {"whisper-large-v3-turbo", "whisper-large-v3"};
    all.push_back(std::move(groq));
    all.push_back(make_endpoint("speaches", "Speaches", "http://127.0.0.1:8000/v1", "",
                                "Systran/faster-whisper-small", false,
                                "A Speaches server. Lists the models it has downloaded."));
    auto localai = make_endpoint("localai", "LocalAI", "http://127.0.0.1:8080/v1", "", "whisper-1",
                                 false, "A LocalAI server with a transcription model installed.");
    localai.models_path = "/models/capabilities";
    all.push_back(std::move(localai));
    all.push_back(make_endpoint("vllm", "vLLM", "http://127.0.0.1:8000/v1", "",
                                "openai/whisper-large-v3-turbo", false,
                                "A vLLM server running a Whisper model (vllm serve)."));
    // whisper-server answers on /inference unless started with --inference-path
    // /v1/audio/transcriptions. It ignores `model`, has no catalog, and assumes English without
    // `language`.
    auto whisper_cpp = make_endpoint(
        "whisper_cpp", "whisper.cpp", "http://127.0.0.1:8080/v1", "", "whisper-1", false,
        "whisper.cpp's whisper-server, started with --inference-path /v1/audio/transcriptions.");
    whisper_cpp.models_path.reset();
    whisper_cpp.auto_language = "auto";
    all.push_back(std::move(whisper_cpp));
    all.push_back(make_endpoint(
        std::string(kCustomEndpointId), "Custom server", std::nullopt, "", "whisper-1", false,
        "Any server with an OpenAI-compatible /audio/transcriptions route."));
    return all;
}

const std::vector<SpeechEndpoint>& all_endpoints() {
    static const std::vector<SpeechEndpoint> endpoints = build_endpoints();
    return endpoints;
}

const SpeechEndpoint* find_endpoint(std::string_view id) {
    for (const auto& endpoint : all_endpoints()) {
        if (endpoint.id == id) {
            return &endpoint;
        }
    }
    return nullptr;
}

const SpeechEndpoint& openai() {
    return all_endpoints().front();
}

Json overrides(const Json& settings, std::string_view endpoint_id) {
    const Json servers = keys::api_transcription_servers.read(settings);
    if (servers.is_object()) {
        const auto entry = servers.find(std::string(endpoint_id));
        if (entry != servers.end() && entry->is_object()) {
            return *entry;
        }
    }
    return Json::object();
}

std::string checked_model(std::string_view model) {
    std::string stripped = py::strip(model);
    if (stripped.empty()) {
        throw std::invalid_argument("Enter a model id.");
    }
    return stripped;
}

void update_server(Json& settings, std::string_view endpoint_id, const Json& values) {
    const SpeechEndpoint* endpoint = find_endpoint(endpoint_id);
    if (endpoint == nullptr || !endpoint->configurable) {
        throw std::invalid_argument(std::string(endpoint_id) + " has no server settings.");
    }
    Json servers = keys::api_transcription_servers.read(settings);
    if (!servers.is_object()) {
        servers = Json::object();
    }
    const std::string id(endpoint_id);
    Json entry = servers.contains(id) && servers[id].is_object() ? servers[id] : Json::object();
    for (const auto& [key, value] : values.items()) {
        entry[key] = value;
    }
    servers[id] = std::move(entry);
    settings[std::string(keys::api_transcription_servers.key())] = std::move(servers);
}

std::string host_of(const std::optional<std::string>& base_url) {
    return detail::urlsplit(base_url.value_or("")).netloc;
}

} // namespace

bool SpeechEndpoint::is_openai() const {
    return id == kOpenAiEndpointId;
}

bool SpeechEndpoint::is_local() const {
    return is_loopback_url(base_url);
}

bool SpeechEndpoint::is_configured() const {
    return is_openai() || (base_url && !base_url->empty());
}

bool SpeechEndpoint::self_hosted() const {
    return (configurable && id != kCustomEndpointId) || is_local();
}

double SpeechEndpoint::timeout_s() const {
    return self_hosted() ? kSelfHostedTimeoutS : kHostedTimeoutS;
}

std::span<const SpeechEndpoint> speech_endpoints() {
    return all_endpoints();
}

std::vector<std::string> endpoint_ids() {
    std::vector<std::string> ids;
    for (const auto& endpoint : all_endpoints()) {
        ids.push_back(endpoint.id);
    }
    return ids;
}

SpeechEndpoint configured_endpoint(std::string_view endpoint_id, const Json& settings) {
    const SpeechEndpoint* found = find_endpoint(endpoint_id);
    const SpeechEndpoint& endpoint = found != nullptr ? *found : openai();
    if (!endpoint.configurable) {
        return endpoint;
    }
    const Json saved = overrides(settings, endpoint.id);
    try {
        SpeechEndpoint configured = endpoint;
        const auto url = saved.find("base_url");
        if (url != saved.end() && url->is_string() && !url->get_ref<const std::string&>().empty()) {
            configured.base_url = valid_server_url(url->get_ref<const std::string&>());
        }
        const auto env = saved.find("api_key_env");
        configured.api_key_env = env != saved.end() && env->is_string()
                                     ? validate_api_key_env(env->get_ref<const std::string&>())
                                     : std::string();
        return configured;
    } catch (const std::invalid_argument&) {
        kLog.warning("Ignoring an invalid saved {} server", endpoint.name);
        return endpoint;
    }
}

SpeechEndpoint selected_endpoint(const Json& settings) {
    SpeechEndpoint endpoint =
        configured_endpoint(keys::api_transcription_endpoint.read(settings), settings);
    return endpoint.is_configured() ? endpoint : openai();
}

std::string endpoint_model(const SpeechEndpoint& endpoint, const Json& settings,
                           std::chrono::year_month_day today) {
    if (endpoint.is_openai()) {
        return resolve_api_transcription_model(settings, today);
    }
    const Json saved = overrides(settings, endpoint.id);
    const auto model = saved.find("model");
    if (model != saved.end() && model->is_string()) {
        std::string stripped = py::strip(model->get_ref<const std::string&>());
        if (!stripped.empty()) {
            return stripped;
        }
    }
    return endpoint.default_model;
}

std::string selected_model(const Json& settings, std::chrono::year_month_day today) {
    return endpoint_model(selected_endpoint(settings), settings, today);
}

std::string normalize_base_url(std::string_view url) {
    const std::string raw = py::strip(url);
    if (raw.empty()) {
        throw std::invalid_argument("Base URL is required");
    }
    detail::UrlParts parts = detail::urlsplit(raw);
    if (parts.scheme != "http" && parts.scheme != "https") {
        throw std::invalid_argument("Base URL must use http or https");
    }
    if (parts.netloc.empty()) {
        throw std::invalid_argument("Base URL is missing a host");
    }
    while (parts.path.ends_with('/')) {
        parts.path.pop_back();
    }
    parts.fragment.clear();
    return detail::urlunsplit(parts);
}

std::string valid_server_url(std::string_view url) {
    std::string normalized = normalize_base_url(url);
    const detail::UrlParts parts = detail::urlsplit(normalized);
    const auto username = parts.username();
    const auto password = parts.password();
    if ((username && !username->empty()) || (password && !password->empty()) ||
        !parts.query.empty()) {
        throw std::invalid_argument("Use a server URL without credentials or query parameters.");
    }
    if (parts.path.empty()) {
        normalized += "/v1";
    }
    return normalized;
}

std::string validate_api_key_env(std::string_view name) {
    std::string cleaned = py::strip(name);
    if (cleaned.empty()) {
        return cleaned;
    }
    const bool valid = (cleaned[0] == '_' || (cleaned[0] >= 'A' && cleaned[0] <= 'Z')) &&
                       std::ranges::all_of(cleaned, [](char c) {
                           return c == '_' || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                       });
    if (!valid) {
        throw std::invalid_argument("API key variable must be empty or an uppercase identifier "
                                    "(A\xE2\x80\x93Z, digits, underscore)");
    }
    return cleaned;
}

bool is_loopback_url(const std::optional<std::string>& url) {
    if (!url || url->empty()) {
        return false;
    }
    try {
        const auto host = detail::urlsplit(*url).hostname().value_or("");
        return std::ranges::find(kLocalHosts, host) != kLocalHosts.end();
    } catch (const std::invalid_argument&) {
        return false;
    }
}

void save_selection(Json& settings, std::string_view endpoint_id) {
    if (find_endpoint(endpoint_id) == nullptr) {
        throw std::invalid_argument("Unknown transcription server: " + std::string(endpoint_id));
    }
    settings[std::string(keys::api_transcription_endpoint.key())] = std::string(endpoint_id);
}

void save_model(Json& settings, std::string_view endpoint_id, std::string_view model) {
    const std::string chosen = checked_model(model);
    if (endpoint_id == kOpenAiEndpointId) {
        if (std::ranges::find(kApiModelChoices, chosen) == kApiModelChoices.end()) {
            throw std::invalid_argument("OpenAI has no transcription model " + chosen + ".");
        }
        settings[std::string(keys::api_transcription_model.key())] = chosen;
        return;
    }
    update_server(settings, endpoint_id, Json{{"model", chosen}});
}

void save_server(Json& settings, std::string_view endpoint_id, std::string_view base_url,
                 std::string_view api_key_env, std::string_view model) {
    const SpeechEndpoint* endpoint = find_endpoint(endpoint_id);
    if (endpoint == nullptr || !endpoint->configurable) {
        throw std::invalid_argument(
            (endpoint != nullptr ? endpoint->name : std::string(endpoint_id)) +
            " can't be edited.");
    }
    const std::string chosen = checked_model(model);
    Json values = Json::object();
    values["base_url"] = valid_server_url(base_url);
    values["api_key_env"] = validate_api_key_env(api_key_env);
    values["model"] = chosen;
    update_server(settings, endpoint_id, values);
}

std::optional<std::string> api_key(const SpeechEndpoint& endpoint, CredentialStore& credentials,
                                   const EnvReader& env, const DotEnv& dotenv) {
    if (endpoint.api_key_env.empty()) {
        return std::string(kAuthFreeApiKey);
    }
    return resolve_credential(credentials, env, dotenv, endpoint.api_key_env).value;
}

std::string destination(const SpeechEndpoint& endpoint) {
    if (endpoint.is_local()) {
        const std::string host = host_of(endpoint.base_url);
        return "your local server at " + (host.empty() ? std::string("localhost") : host);
    }
    if (endpoint.kind == "openai") {
        return "OpenAI (api.openai.com)";
    }
    const std::string host = host_of(endpoint.base_url);
    return endpoint.name + " (" + (host.empty() ? endpoint.name : host) + ")";
}

std::string audio_destination_sentence(const SpeechEndpoint& endpoint) {
    if (endpoint.is_local()) {
        return "Audio stays on this computer: it goes to " + destination(endpoint) + ".";
    }
    return "Audio is uploaded to " + destination(endpoint) + ".";
}

std::vector<std::pair<std::string, std::string>> credential_owners(const Json& settings) {
    std::vector<std::pair<std::string, std::string>> owners;
    for (const auto& preset : all_endpoints()) {
        const SpeechEndpoint endpoint = configured_endpoint(preset.id, settings);
        if (!endpoint.api_key_env.empty() && endpoint.is_configured()) {
            owners.emplace_back(endpoint.api_key_env, endpoint.name);
        }
    }
    return owners;
}

bool is_speech_model(const Json& item) {
    const auto task = item.find("task");
    if (task != item.end() && !task->is_null() &&
        !(task->is_string() && task->get_ref<const std::string&>() == kSpeechTask)) {
        return false;
    }
    const auto capabilities = item.find("capabilities");
    if (capabilities == item.end() || !capabilities->is_array()) {
        return true;
    }
    return std::ranges::any_of(*capabilities, [](const Json& capability) {
        return capability.is_string() &&
               capability.get_ref<const std::string&>() == kSpeechCapability;
    });
}

std::vector<std::string> list_models(http::Client& client, const SpeechEndpoint& endpoint,
                                     const std::optional<std::string>& key,
                                     const CancelToken& cancel, std::chrono::year_month_day today) {
    if (endpoint.is_openai()) {
        return api_model_choices(today);
    }
    if (!endpoint.models_path) {
        return endpoint.known_models;
    }
    http::Request request;
    request.url = endpoint.base_url.value_or("") + *endpoint.models_path;
    if (key && !key->empty()) {
        request.headers.emplace_back("Authorization", "Bearer " + *key);
    }
    request.connect_timeout = kCatalogTimeout;
    request.idle_timeout = kCatalogTimeout;
    const auto response = client.send(request, cancel);
    if (!response) {
        throw std::runtime_error(response.error().message);
    }
    if (response->status == 404) {
        return {};
    }
    if (response->status < 200 || response->status >= 300) {
        throw SpeechApiError(endpoint.name + " answered HTTP " + std::to_string(response->status),
                             response->status, std::nullopt);
    }
    const Json body = py::loads(response->body);
    if (!body.is_object()) {
        throw std::runtime_error("The model catalog is not a JSON object");
    }
    std::vector<std::string> ids;
    const auto data = body.find("data");
    if (data != body.end() && data->is_array()) {
        for (const Json& item : *data) {
            if (!item.is_object()) {
                continue;
            }
            const auto id = item.find("id");
            if (id != item.end() && id->is_string() && !id->get_ref<const std::string&>().empty() &&
                is_speech_model(item)) {
                ids.push_back(id->get<std::string>());
            }
        }
    }
    // Python sorts by code point, which is UTF-8 byte order.
    std::ranges::sort(ids);
    return ids;
}

std::vector<std::string> list_models(http::Client& client, const SpeechEndpoint& endpoint,
                                     const std::optional<std::string>& key,
                                     const CancelToken& cancel) {
    return list_models(client, endpoint, key, cancel, local_today(system_clock()));
}

} // namespace fv::speech
