#include "llm/catalog_client.h"

#include <algorithm>
#include <array>
#include <format>

#include "base/log.h"
#include "llm/detail/keys.h"
#include "llm/detail/py_repr.h"
#include "llm/detail/sdk_request.h"
#include "llm/detail/truthy.h"
#include "llm/detail/url.h"
#include "llm/local_servers.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::llm {

namespace {

constexpr log::Logger kLog{"services.text_llm"};

constexpr auto kVerifyTimeout = std::chrono::milliseconds(10'000);
constexpr auto kCatalogTimeout = std::chrono::milliseconds(15'000);
constexpr std::string_view kOpenAiBaseUrl = "https://api.openai.com/v1";

constexpr std::array<std::string_view, 5> kOpenAiChatPrefixes{"gpt-", "chatgpt-", "o1", "o3", "o4"};
constexpr std::array<std::string_view, 11> kOpenAiNonChatMarkers{
    "audio",  "realtime",   "tts",   "whisper", "embedding", "moderation",
    "dall-e", "transcribe", "image", "search",  "instruct"};
// Groq lists speech, safety and agent models beside its chat models.
constexpr std::array<std::string_view, 5> kGroqNonChatMarkers{"whisper", "tts", "orpheus", "guard",
                                                              "compound"};

template <class Markers> bool contains_any(std::string_view text, const Markers& markers) {
    return std::ranges::any_of(
        markers, [&](std::string_view m) { return text.find(m) != std::string_view::npos; });
}

std::string host_of(const TextLLMProfile& profile) {
    if (!profile.base_url) {
        return "api.openai.com";
    }
    try {
        return detail::urlsplit(*profile.base_url).hostname().value_or("the endpoint");
    } catch (const std::invalid_argument&) {
        return "the endpoint";
    }
}

// The SDK client's request: {base}{path} with the key, JSON accepted, the provider's headers.
http::Request sdk_request(const TextLLMProfile& profile, std::string_view key,
                          std::string_view path, std::chrono::milliseconds timeout) {
    http::Request request;
    request.url = profile.base_url.value_or(std::string(kOpenAiBaseUrl));
    while (request.url.ends_with('/')) {
        request.url.pop_back();
    }
    request.url += path;
    request.headers.emplace_back("Authorization", "Bearer " + std::string(key));
    request.headers.emplace_back("Accept", "application/json");
    if (const auto extra = provider_headers(profile)) {
        for (const auto& header : *extra) {
            request.headers.push_back(header);
        }
    }
    request.connect_timeout = timeout;
    request.idle_timeout = timeout;
    request.follow_redirects = true;
    return request;
}

std::string_view status_error_class(int status) {
    switch (status) {
    case 400:
        return "BadRequestError";
    case 401:
        return "AuthenticationError";
    case 403:
        return "PermissionDeniedError";
    case 404:
        return "NotFoundError";
    case 409:
        return "ConflictError";
    case 422:
        return "UnprocessableEntityError";
    case 429:
        return "RateLimitError";
    default:
        return status >= 500 ? "InternalServerError" : "APIStatusError";
    }
}

// The SDK's model page: a JSON object. Any other answer fails as the SDK's parse does (Python's
// AttributeError on the text it got).
std::optional<Json> model_page(const std::string& body) {
    try {
        Json page = py::loads(body);
        if (page.is_object()) {
            return page;
        }
    } catch (const std::exception&) {
        // Not JSON: the SDK hands the page builder the text.
    }
    return std::nullopt;
}

// The query value as httpx encodes it (urlencode: a space is "+").
std::string query_quote(std::string_view value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else if (c == ' ') {
            out += '+';
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

} // namespace

KeyCheck verify_api_key(http::Client& client, const TextLLMProfile& profile, std::string_view key,
                        const CancelToken& cancel, KeySources* keys) {
    return verify_api_key(client, profile, key, cancel, keys, detail::sleeping_wait());
}

KeyCheck verify_api_key(http::Client& client, const TextLLMProfile& profile, std::string_view key,
                        const CancelToken& cancel, KeySources* keys, const RetryWait& wait) {
    const std::string host = host_of(profile);
    const auto sent = detail::key_for(profile, key, keys);
    if (!sent || sent->empty()) {
        // create_openai_client: "No API key found for <name> (set <variable>)".
        kLog.debug("Key verification failed: {}", "RuntimeError");
        return {false, "Verification failed (RuntimeError)."};
    }
    // OpenRouter's model catalog is public, so listing it proves nothing about the key;
    // /auth/key requires it.
    const auto request = sdk_request(
        profile, *sent, profile.kind == kProfileKindOpenRouter ? "/auth/key" : "/models",
        kVerifyTimeout);
    std::expected<http::Response, http::Error> response;
    try {
        response = detail::sdk_send(client, request, cancel, wait);
    } catch (const std::exception& error) {
        // Only the exception's class: its text may hold the key.
        const auto* python = dynamic_cast<const py::PyError*>(&error);
        const std::string type = python != nullptr ? std::string(python->type_name())
                                 : dynamic_cast<const std::invalid_argument*>(&error) != nullptr
                                     ? "ValueError"
                                     : "Exception";
        kLog.debug("Key verification failed: {}", type);
        return {false, "Verification failed (" + type + ")."};
    }
    if (!response) {
        if (response.error().kind == http::ErrorKind::Cancelled || cancel.cancelled()) {
            kLog.debug("Key verification failed: {}", "Cancelled");
            return {false, "Verification failed (Cancelled)."};
        }
        return {false, std::format("Couldn't reach {}.", host)};
    }
    const int status = response->status;
    if (status == 401) {
        return {false, std::format("{} rejected the key (HTTP 401).", host)};
    }
    if (status == 403) {
        return {false, std::format("{} accepted the key but denied access (HTTP 403).", host)};
    }
    if (status < 200 || status >= 300) {
        return {false, std::format("{} answered HTTP {}.", host, status)};
    }
    if (profile.kind != kProfileKindOpenRouter && !model_page(response->body)) {
        kLog.debug("Key verification failed: {}", "AttributeError");
        return {false, "Verification failed (AttributeError)."};
    }
    if (profile.kind == "opencode_go" || profile.kind == "opencode_zen") {
        return {true, "Catalog reachable. OpenCode's public catalog cannot verify a key; a model "
                      "request is required."};
    }
    return {true, std::format("{} accepted the key.", host)};
}

std::vector<std::string> filter_openai_chat_models(const std::vector<std::string>& model_ids) {
    std::vector<std::string> filtered;
    for (const auto& model_id : model_ids) {
        const std::string lowered = py::lower(model_id);
        const bool chat = std::ranges::any_of(
            kOpenAiChatPrefixes, [&](std::string_view p) { return lowered.starts_with(p); });
        if (!chat || contains_any(lowered, kOpenAiNonChatMarkers)) {
            continue;
        }
        filtered.push_back(model_id);
    }
    return filtered;
}

std::vector<std::string> list_chat_models(http::Client& client, const TextLLMProfile& profile,
                                          std::string_view key, std::string_view sort,
                                          const CancelToken& cancel, KeySources* keys) {
    return list_chat_models(client, profile, key, sort, cancel, keys, detail::sleeping_wait());
}

std::vector<std::string> list_chat_models(http::Client& client, const TextLLMProfile& profile,
                                          std::string_view key, std::string_view sort,
                                          const CancelToken& cancel, KeySources* keys,
                                          const RetryWait& wait) {
    if (profile.self_hosted()) {
        return discover_server_models(client, profile, key, keys, kCatalogTimeout, cancel);
    }
    const auto sent = detail::key_for(profile, key, keys);
    if (!sent || sent->empty()) {
        throw CatalogError("RuntimeError", "No API key found for " + profile.name + " (set " +
                                               profile.api_key_env + ")");
    }
    const bool server_sort =
        profile.kind == kProfileKindOpenRouter && !sort.empty() && sort != "alphabetical";
    std::string path = "/models";
    if (server_sort) {
        path += "?sort=" + query_quote(sort);
    }
    const auto response =
        detail::sdk_send(client, sdk_request(profile, *sent, path, kCatalogTimeout), cancel, wait);
    if (!response) {
        const auto& error = response.error();
        if (error.kind == http::ErrorKind::Cancelled || cancel.cancelled()) {
            throw CatalogError("Cancelled", "canceled");
        }
        if (error.kind == http::ErrorKind::Timeout) {
            throw CatalogError("APITimeoutError", "Request timed out.");
        }
        throw CatalogError("APIConnectionError", "Connection error.");
    }
    if (response->status < 200 || response->status >= 300) {
        throw CatalogError(status_error_class(response->status),
                           detail::sdk_status_message(*response));
    }
    const auto page = model_page(response->body);
    if (!page) {
        throw CatalogError("AttributeError",
                           "'str' object has no attribute '_set_private_attributes'");
    }
    std::vector<std::string> model_ids;
    if (const Json& data = detail::get(*page, "data"); data.is_array()) {
        for (const auto& model : data) {
            if (!model.is_object()) {
                throw CatalogError("AttributeError", "'" +
                                                         std::string(detail::py_type_name(model)) +
                                                         "' object has no attribute 'id'");
            }
            if (const auto* id = detail::text(detail::get(model, "id"))) {
                model_ids.push_back(*id);
            }
        }
    }
    if (server_sort) {
        return model_ids;
    }
    if (profile.kind == kProfileKindOpenAi) {
        model_ids = filter_openai_chat_models(model_ids);
    }
    if (profile.kind == "groq") {
        std::erase_if(model_ids, [](const std::string& model) {
            return contains_any(py::lower(model), kGroqNonChatMarkers);
        });
    }
    std::ranges::sort(model_ids);
    return model_ids;
}

} // namespace fv::llm
