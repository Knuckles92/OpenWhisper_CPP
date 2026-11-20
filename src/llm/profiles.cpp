#include "llm/profiles.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "base/ids.h"
#include "llm/cleanup_settings.h"
#include "llm/detail/py_repr.h"
#include "llm/detail/truthy.h"
#include "llm/detail/url.h"
#include "llm/model_catalog.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "settings/keys.h"

namespace fv::llm {

namespace {

constexpr std::string_view kCustomIdPrefix = "custom_";

bool is_env_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    const auto first = name.front();
    if (!((first >= 'A' && first <= 'Z') || first == '_')) {
        return false;
    }
    return std::ranges::all_of(
        name, [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; });
}

// str(value or ""): Python's str() of a truthy value, "" for a falsy one.
std::string str_or_empty(const Json& value) {
    return detail::truthy(value) ? detail::py_str(value) : std::string();
}

std::pair<std::string, std::string> local_server_keys(std::string_view profile_id) {
    return {std::string(profile_id) + "_base_url", std::string(profile_id) + "_api_key_env"};
}

TextLLMProfile configured_builtin(const TextLLMProfile& profile, const Json& settings) {
    const auto default_url = local_server_default_url(profile.id);
    if (!default_url) {
        return profile;
    }
    const auto [url_key, env_key] = local_server_keys(profile.id);
    std::string api_key_env;
    try {
        api_key_env = validate_api_key_env(str_or_empty(detail::get(settings, env_key)));
    } catch (const std::invalid_argument&) {
        api_key_env.clear();
    }
    std::string base_url;
    const Json& saved = detail::get(settings, url_key);
    if (!detail::truthy(saved)) {
        base_url = normalize_base_url(*default_url);
    } else if (const auto* text = detail::text(saved)) {
        try {
            base_url = normalize_base_url(*text);
        } catch (const std::invalid_argument&) {
            // A hand-edited address must not take every profile lookup down.
            base_url = normalize_base_url(*default_url);
        }
    } else {
        // Python's AttributeError on .strip() of a number, list or mapping.
        base_url = normalize_base_url(*default_url);
    }
    TextLLMProfile configured = profile;
    configured.base_url = std::move(base_url);
    configured.api_key_env = std::move(api_key_env);
    return configured;
}

std::string url_netloc(const std::optional<std::string>& url) {
    return detail::urlsplit(url.value_or("")).netloc;
}

} // namespace

bool is_local_server_id(std::string_view id) {
    return local_server_default_url(id).has_value();
}

std::optional<std::string_view> local_server_default_url(std::string_view id) {
    for (const auto& server : kLocalServers) {
        if (server.id == id) {
            return server.default_url;
        }
    }
    return std::nullopt;
}

bool is_builtin_kind(std::string_view kind) {
    return std::ranges::find(kBuiltinKinds, kind) != kBuiltinKinds.end();
}

bool is_profile_kind(std::string_view kind) {
    return is_builtin_kind(kind) || kind == kProfileKindCustom;
}

bool is_loopback_url(std::optional<std::string_view> url) {
    if (!url || url->empty()) {
        return false;
    }
    const auto host = py::lower(detail::urlsplit(*url).hostname().value_or(""));
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

bool TextLLMProfile::is_local() const {
    return is_loopback_url(base_url ? std::optional<std::string_view>(*base_url) : std::nullopt);
}

bool TextLLMProfile::self_hosted() const {
    return is_local_server_id(kind) || is_local();
}

bool TextLLMProfile::refuses_unsupported_tools() const {
    return kind == kProfileKindOpenAi || kind == kProfileKindOpenRouter;
}

bool TextLLMProfile::operator==(const TextLLMProfile& other) const {
    const bool same_metadata =
        model_metadata.has_value() == other.model_metadata.has_value() &&
        (!model_metadata || py::py_equal(*model_metadata, *other.model_metadata));
    return id == other.id && name == other.name && kind == other.kind &&
           base_url == other.base_url && api_key_env == other.api_key_env &&
           builtin == other.builtin && same_metadata && protocol == other.protocol;
}

Json TextLLMSnapshot::to_dict() const {
    Json out = Json::object();
    out["profile_id"] = profile_id;
    out["name"] = name;
    out["kind"] = kind;
    out["base_url"] = base_url ? Json(*base_url) : Json(nullptr);
    out["api_key_env"] = api_key_env;
    if (model_metadata && detail::truthy(*model_metadata)) {
        out["model_metadata"] = *model_metadata;
    }
    if (protocol != "chat") {
        out["protocol"] = protocol;
    }
    return out;
}

TextLLMProfile TextLLMSnapshot::to_profile() const {
    TextLLMProfile profile;
    profile.id = profile_id;
    profile.name = name;
    profile.kind = is_profile_kind(kind) ? kind : std::string(kProfileKindCustom);
    profile.base_url = base_url;
    profile.api_key_env = api_key_env;
    profile.model_metadata = model_metadata;
    profile.protocol = protocol;
    profile.builtin = is_builtin_kind(profile_id) && kind == profile_id;
    return profile;
}

const std::vector<TextLLMProfile>& builtin_profiles() {
    static const std::vector<TextLLMProfile> profiles = [] {
        std::vector<TextLLMProfile> list;
        auto add = [&list](std::string_view id, std::string_view name,
                           std::optional<std::string> base_url, std::string_view env) {
            TextLLMProfile profile;
            profile.id = std::string(id);
            profile.name = std::string(name);
            profile.kind = std::string(id);
            profile.base_url = std::move(base_url);
            profile.api_key_env = std::string(env);
            profile.builtin = true;
            list.push_back(std::move(profile));
        };
        add(kOpenAiProfileId, "OpenAI", std::nullopt, "OPENAI_API_KEY");
        add(kOpenRouterProfileId, "OpenRouter", std::string(kOpenRouterBaseUrl),
            "OPENROUTER_API_KEY");
        for (const auto& server : kLocalServers) {
            add(server.id, server.name, std::string(server.default_url), "");
        }
        add("groq", "Groq", "https://api.groq.com/openai/v1", "GROQ_API_KEY");
        add("opencode_go", "OpenCode Go", "https://opencode.ai/zen/go/v1", "OPENCODE_GO_API_KEY");
        add("opencode_zen", "OpenCode Zen", "https://opencode.ai/zen/v1", "OPENCODE_ZEN_API_KEY");
        return list;
    }();
    return profiles;
}

std::optional<TextLLMProfile> builtin_profile(std::string_view profile_id) {
    for (const auto& profile : builtin_profiles()) {
        if (profile.id == profile_id) {
            return profile;
        }
    }
    return std::nullopt;
}

std::string normalize_base_url(std::string_view url) {
    const std::string raw = py::strip(url);
    if (raw.empty()) {
        throw std::invalid_argument("Base URL is required");
    }
    auto parts = detail::urlsplit(raw);
    if (parts.scheme != "http" && parts.scheme != "https") {
        throw std::invalid_argument("Base URL must use http or https");
    }
    if (parts.netloc.empty()) {
        throw std::invalid_argument("Base URL is missing a host");
    }
    while (!parts.path.empty() && parts.path.back() == '/') {
        parts.path.pop_back();
    }
    parts.fragment.clear();
    return detail::urlunsplit(parts);
}

std::string validate_profile_name(std::string_view name) {
    std::string cleaned = py::strip(name);
    if (cleaned.empty()) {
        throw std::invalid_argument("Endpoint name is required");
    }
    if (py::len(cleaned) > kMaxProfileNameLen) {
        throw std::invalid_argument("Endpoint name must be 80 characters or fewer");
    }
    return cleaned;
}

std::string validate_api_key_env(std::string_view name) {
    std::string cleaned = py::strip(name);
    if (cleaned.empty()) {
        return cleaned;
    }
    if (!is_env_name(cleaned)) {
        throw std::invalid_argument("API key variable must be empty or an uppercase identifier "
                                    "(A–Z, digits, underscore)");
    }
    return cleaned;
}

std::string new_custom_profile_id() {
    return std::string(kCustomIdPrefix) + random_hex(8);
}

std::string validate_text_protocol(const Json& protocol) {
    const auto* text = detail::text(protocol);
    if (text == nullptr || (*text != "chat" && *text != "responses")) {
        throw std::invalid_argument("Choose Chat Completions or Responses for this endpoint.");
    }
    return *text;
}

TextLLMSnapshot snapshot_from_profile(const TextLLMProfile& profile, std::string_view model) {
    std::optional<Json> metadata = profile.model_metadata;
    if (!model.empty()) {
        Json saved = Json::object();
        saved["model"] = std::string(model);
        const auto defaults = model_spec(profile, model).to_dict();
        for (const auto& [key, value] : defaults.items()) {
            saved[key] = value;
        }
        metadata = std::move(saved);
    }
    TextLLMSnapshot snapshot;
    snapshot.profile_id = profile.id;
    snapshot.name = profile.name;
    snapshot.kind = profile.kind;
    snapshot.base_url = profile.base_url;
    snapshot.api_key_env = profile.api_key_env;
    snapshot.model_metadata = std::move(metadata);
    snapshot.protocol = profile.protocol;
    return snapshot;
}

namespace {

std::optional<Json> decoded_mapping(const Json& raw) {
    if (const auto* text = detail::text(raw); text != nullptr && !py::strip(*text).empty()) {
        try {
            Json decoded = py::loads(*text);
            if (decoded.is_object()) {
                return decoded;
            }
        } catch (const py::PyError&) {
            // json.loads' ValueError or RecursionError: not a snapshot.
        }
        return std::nullopt;
    }
    if (!raw.is_object()) {
        return std::nullopt;
    }
    return std::optional<Json>(raw);
}

std::string saved_api_key_env(const Json& api_key_env) {
    const auto* text = detail::text(api_key_env);
    if (text == nullptr) {
        return {};
    }
    try {
        return validate_api_key_env(*text);
    } catch (const std::invalid_argument&) {
        return {};
    }
}

std::optional<Json> saved_model_metadata(const Json& metadata) {
    if (metadata.is_null()) {
        return std::nullopt;
    }
    if (!metadata.is_object() || !detail::get(metadata, "model").is_string()) {
        throw std::invalid_argument("Invalid saved text model metadata");
    }
    Json saved = Json::object();
    saved["model"] = metadata.at("model");
    const auto fields = spec_from_mapping(metadata).to_dict();
    for (const auto& [key, value] : fields.items()) {
        saved[key] = value;
    }
    return saved;
}

} // namespace

std::optional<TextLLMSnapshot> snapshot_from_mapping(const Json& input) {
    const auto decoded = decoded_mapping(input);
    if (!decoded) {
        return std::nullopt;
    }
    const Json& raw = *decoded;
    const Json& profile_id = detail::truthy(detail::get(raw, "profile_id"))
                                 ? detail::get(raw, "profile_id")
                                 : detail::get(raw, "id");
    const auto* id_text = detail::text(profile_id);
    if (id_text == nullptr || py::strip(*id_text).empty()) {
        return std::nullopt;
    }
    const Json& name_value = detail::get(raw, "name");
    std::string name;
    if (const auto* text = detail::text(name_value); text != nullptr && !py::strip(*text).empty()) {
        name = *text;
    } else {
        name = *id_text;
    }
    std::string kind(kProfileKindCustom);
    if (const auto* text = detail::text(detail::get(raw, "kind"));
        text != nullptr && is_profile_kind(*text)) {
        kind = *text;
    }
    std::optional<std::string> url;
    if (const auto* text = detail::text(detail::get(raw, "base_url"));
        text != nullptr && !py::strip(*text).empty()) {
        try {
            url = normalize_base_url(*text);
        } catch (const std::invalid_argument&) {
            return std::nullopt;
        }
    }
    if (kind != kProfileKindOpenAi && !url) {
        return std::nullopt;
    }
    const std::string api_key_env = saved_api_key_env(detail::get(raw, "api_key_env"));
    const std::string protocol =
        validate_text_protocol(detail::has(raw, "protocol") ? raw.at("protocol") : Json("chat"));
    auto metadata = saved_model_metadata(detail::get(raw, "model_metadata"));
    TextLLMSnapshot snapshot;
    snapshot.model_metadata = std::move(metadata);
    snapshot.protocol = protocol;
    snapshot.profile_id = py::strip(*id_text);
    snapshot.name = py::strip(name);
    snapshot.kind = kind;
    snapshot.base_url = std::move(url);
    snapshot.api_key_env = api_key_env;
    return snapshot;
}

std::optional<TextLLMProfile> parse_custom_profile(const Json& raw) {
    if (!raw.is_object()) {
        return std::nullopt;
    }
    const auto* id = detail::text(detail::get(raw, "id"));
    if (id == nullptr || !id->starts_with(kCustomIdPrefix)) {
        return std::nullopt;
    }
    TextLLMProfile profile;
    try {
        profile.name = validate_profile_name(str_or_empty(detail::get(raw, "name")));
        profile.base_url = normalize_base_url(str_or_empty(detail::get(raw, "base_url")));
        profile.api_key_env = validate_api_key_env(str_or_empty(detail::get(raw, "api_key_env")));
        profile.protocol = validate_text_protocol(detail::has(raw, "protocol") ? raw.at("protocol")
                                                                               : Json("chat"));
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
    profile.id = *id;
    profile.kind = std::string(kProfileKindCustom);
    profile.builtin = false;
    return profile;
}

std::vector<TextLLMProfile> list_custom_profiles(const Json& settings) {
    std::vector<TextLLMProfile> profiles;
    const Json payload = keys::text_llm_profiles.read(settings);
    for (const auto& raw : payload) {
        auto profile = parse_custom_profile(raw);
        if (!profile || std::ranges::any_of(profiles, [&](const TextLLMProfile& seen) {
                return seen.id == profile->id;
            })) {
            continue;
        }
        profiles.push_back(std::move(*profile));
        if (profiles.size() >= kMaxCustomProfiles) {
            break;
        }
    }
    return profiles;
}

std::vector<TextLLMProfile> list_profiles(const Json& settings) {
    std::vector<TextLLMProfile> profiles;
    for (const auto& profile : builtin_profiles()) {
        profiles.push_back(configured_builtin(profile, settings));
    }
    for (auto& profile : list_custom_profiles(settings)) {
        profiles.push_back(std::move(profile));
    }
    return profiles;
}

std::string normalize_local_server_url(std::string_view url) {
    const std::string normalized = normalize_base_url(url);
    const auto parts = detail::urlsplit(normalized);
    if (parts.username() || parts.password() || !parts.query.empty()) {
        throw std::invalid_argument("Use a server URL without credentials or query parameters.");
    }
    return parts.path.empty() ? normalized + "/v1" : normalized;
}

void save_local_server(Json& settings, std::string_view profile_id, std::string_view url,
                       std::string_view api_key_env) {
    if (!is_local_server_id(profile_id)) {
        throw std::invalid_argument("'" + std::string(profile_id) +
                                    "' is not a built-in local server.");
    }
    const auto [url_key, env_key] = local_server_keys(profile_id);
    std::string normalized = normalize_local_server_url(url);
    std::string env = validate_api_key_env(api_key_env);
    if (!settings.is_object()) {
        settings = Json::object();
    }
    settings[url_key] = std::move(normalized);
    settings[env_key] = std::move(env);
}

std::optional<TextLLMProfile> get_profile(std::optional<std::string_view> profile_id,
                                          const Json& settings) {
    if (!profile_id) {
        return std::nullopt;
    }
    const std::string id = py::strip(*profile_id);
    if (id.empty()) {
        return std::nullopt;
    }
    if (auto builtin = builtin_profile(id)) {
        return configured_builtin(*builtin, settings);
    }
    for (auto& profile : list_custom_profiles(settings)) {
        if (profile.id == id) {
            return std::move(profile);
        }
    }
    return std::nullopt;
}

std::vector<std::string> known_profile_ids(const Json& settings) {
    std::vector<std::string> ids;
    for (const auto& profile : list_profiles(settings)) {
        ids.push_back(profile.id);
    }
    return ids;
}

Json custom_profiles_payload(const std::vector<TextLLMProfile>& profiles) {
    Json payload = Json::array();
    for (const auto& profile : profiles) {
        if (profile.builtin || profile.kind != kProfileKindCustom) {
            continue;
        }
        Json entry = Json::object();
        entry["id"] = profile.id;
        entry["name"] = profile.name;
        entry["base_url"] = profile.base_url.value_or("");
        entry["api_key_env"] = profile.api_key_env;
        entry["protocol"] = profile.protocol;
        payload.push_back(std::move(entry));
        if (payload.size() >= kMaxCustomProfiles) {
            break;
        }
    }
    return payload;
}

TextLLMProfile upsert_custom_profile(Json& settings, std::string_view name,
                                     std::string_view base_url, std::string_view api_key_env,
                                     std::optional<std::string_view> profile_id,
                                     std::optional<std::string_view> protocol) {
    const std::string clean_name = validate_profile_name(name);
    const std::string clean_url = normalize_base_url(base_url);
    const std::string clean_env = validate_api_key_env(api_key_env);
    auto existing = list_custom_profiles(settings);
    const TextLLMProfile* previous = nullptr;
    if (profile_id) {
        for (const auto& profile : existing) {
            if (profile.id == *profile_id) {
                previous = &profile;
                break;
            }
        }
    }
    const std::string clean_protocol =
        validate_text_protocol(protocol   ? Json(std::string(*protocol))
                               : previous ? Json(previous->protocol)
                                          : Json("chat"));
    auto custom = [&](std::string id) {
        TextLLMProfile profile;
        profile.id = std::move(id);
        profile.name = clean_name;
        profile.kind = std::string(kProfileKindCustom);
        profile.base_url = clean_url;
        profile.api_key_env = clean_env;
        profile.protocol = clean_protocol;
        return profile;
    };
    TextLLMProfile result;
    std::vector<TextLLMProfile> updated;
    if (profile_id && !profile_id->empty()) {
        if (!get_profile(*profile_id, settings) || builtin_profile(*profile_id)) {
            throw std::invalid_argument("Cannot edit a built-in endpoint");
        }
        bool found = false;
        for (auto& profile : existing) {
            if (profile.id == *profile_id) {
                updated.push_back(custom(std::string(*profile_id)));
                if (!found) {
                    result = updated.back();
                    found = true;
                }
            } else {
                updated.push_back(std::move(profile));
            }
        }
        if (!found) {
            // next() over no match: Python's StopIteration (an id that only matches stripped).
            throw std::runtime_error("StopIteration");
        }
    } else {
        if (existing.size() >= kMaxCustomProfiles) {
            throw std::invalid_argument("At most 50 custom endpoints can be saved");
        }
        result = custom(new_custom_profile_id());
        updated = std::move(existing);
        updated.push_back(result);
    }
    if (!settings.is_object()) {
        settings = Json::object();
    }
    settings[std::string(keys::text_llm_profiles.key())] = custom_profiles_payload(updated);
    return result;
}

bool remove_custom_profile(Json& settings, std::string_view profile_id) {
    if (builtin_profile(profile_id)) {
        throw std::invalid_argument("Built-in endpoints cannot be deleted");
    }
    const auto all = list_custom_profiles(settings);
    std::vector<TextLLMProfile> remaining;
    for (const auto& profile : all) {
        if (profile.id != profile_id) {
            remaining.push_back(profile);
        }
    }
    const bool removed = remaining.size() != all.size();
    if (!settings.is_object()) {
        settings = Json::object();
    }
    settings[std::string(keys::text_llm_profiles.key())] = custom_profiles_payload(remaining);
    return removed;
}

std::string profile_display_name(std::optional<std::string_view> profile_id, const Json& settings) {
    if (profile_id) {
        if (*profile_id == "claude_code") {
            return "Claude Code";
        }
        if (*profile_id == "codex") {
            return "Codex";
        }
        if (*profile_id == "opencode_cli") {
            return "OpenCode";
        }
    }
    if (auto profile = get_profile(profile_id, settings)) {
        return profile->name;
    }
    return profile_id ? py::strip(*profile_id) : std::string();
}

std::string default_model_for_profile(const TextLLMProfile& profile) {
    if (profile.id == kOpenRouterProfileId || profile.kind == kProfileKindOpenRouter) {
        return std::string(kTranscriptCleanupOpenRouterModel);
    }
    if (profile.id == kOpenAiProfileId || profile.kind == kProfileKindOpenAi) {
        return std::string(kTranscriptCleanupModel);
    }
    return {};
}

Json chat_request_options(const TextLLMProfile& profile, std::string_view reasoning) {
    if (reasoning == "off" ||
        (profile.kind != kProfileKindOpenAi && profile.kind != kProfileKindOpenRouter)) {
        return Json{{"temperature", 0}};
    }
    if (profile.kind == kProfileKindOpenRouter) {
        return Json{{"extra_body", {{"reasoning", {{"effort", std::string(reasoning)}}}}}};
    }
    return Json{{"reasoning_effort", std::string(reasoning)}};
}

std::string consent_destination(const TextLLMProfile& profile) {
    if (profile.is_local()) {
        std::string host = url_netloc(profile.base_url);
        if (host.empty()) {
            host = "localhost";
        }
        return "your local server at " + host;
    }
    if (profile.kind == kProfileKindOpenRouter) {
        return "OpenRouter (openrouter.ai)";
    }
    if (profile.kind == kProfileKindOpenAi) {
        return "OpenAI (api.openai.com)";
    }
    std::string host = url_netloc(profile.base_url);
    if (host.empty()) {
        host = profile.name;
    }
    return profile.name + " (" + host + ")";
}

bool destination_is_remote(const TextLLMProfile& profile) {
    return !profile.is_local();
}

TextLLMProfile profile_or_meeting_default(std::optional<std::string_view> provider,
                                          const Json& settings) {
    if (auto profile = get_profile(provider, settings)) {
        return std::move(*profile);
    }
    return resolve_meeting_llm_profile(settings);
}

TextLLMSnapshot snapshot_from_meeting(const Json& meeting, const Json& settings,
                                      std::string_view fallback_provider) {
    if (auto snapshot = snapshot_from_mapping(detail::get(meeting, "agent_endpoint_json"))) {
        return std::move(*snapshot);
    }
    const Json& saved = detail::get(meeting, "agent_provider");
    std::optional<std::string_view> provider;
    if (!detail::truthy(saved)) {
        provider = fallback_provider;
    } else if (const auto* text = detail::text(saved)) {
        provider = *text;
    }
    return snapshot_from_profile(profile_or_meeting_default(provider, settings));
}

TextLLMProfile profile_from_agent_config(std::string_view provider, const Json& endpoint,
                                         const Json& settings) {
    if (auto snapshot = snapshot_from_mapping(endpoint)) {
        return snapshot->to_profile();
    }
    return profile_or_meeting_default(provider, settings);
}

} // namespace fv::llm
