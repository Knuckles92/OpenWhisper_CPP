#include "llm/local_servers.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#include "base/log.h"
#include "llm/catalog_client.h"
#include "llm/cleanup_settings.h"
#include "llm/detail/keys.h"
#include "llm/detail/py_repr.h"
#include "llm/detail/truthy.h"
#include "llm/detail/url.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/regex.h"

namespace fv::llm {

namespace {

constexpr log::Logger kLog{"services.local_text_servers"};
constexpr log::Logger kTextLlmLog{"services.text_llm"};

// Ollama's /api/show runs four at a time, as OpenWhisper's thread pool does.
constexpr std::size_t kShowWorkers = 4;

// Where OpenWhisper's code raises on an answer of the wrong shape, with Python's exception class
// (a string literal): a describer then fails as a whole, and the model list or identification
// fails with it.
[[noreturn]] void shape_error(std::string_view type, const std::string& message) {
    throw CatalogError(type, message);
}

[[noreturn]] void not_iterable(const Json& value) {
    shape_error("TypeError",
                "'" + std::string(detail::py_type_name(value)) + "' object is not iterable");
}

[[noreturn]] void unhashable(const Json& value) {
    shape_error("TypeError", "unhashable type: '" + std::string(detail::py_type_name(value)) + "'");
}

// mapping.get(key) on a value Python requires to be a dict.
const Json& dict_get(const Json& mapping, std::string_view key) {
    if (!mapping.is_object()) {
        shape_error("AttributeError", "'" + std::string(detail::py_type_name(mapping)) +
                                          "' object has no attribute 'get'");
    }
    return detail::get(mapping, key);
}

// `needle in container` for a JSON container: list items, dict keys, or a substring.
bool py_in(std::string_view needle, const Json& container) {
    if (container.is_array()) {
        return std::ranges::any_of(container, [&](const Json& item) {
            return item.is_string() && item.get_ref<const std::string&>() == needle;
        });
    }
    if (container.is_object()) {
        return container.find(needle) != container.end();
    }
    if (const auto* text = detail::text(container)) {
        return text->find(needle) != std::string::npos;
    }
    shape_error("TypeError", "argument of type '" + std::string(detail::py_type_name(container)) +
                                 "' is not iterable");
}

// `value or []` iterated: the items of a list, nothing for a falsy value. A non-empty string or
// mapping iterates as characters or keys, whose .get then fails, as Python's would.
const Json& list_or_empty(const Json& value) {
    static const Json empty = Json::array();
    if (!detail::truthy(value)) {
        return empty;
    }
    if (value.is_string() || value.is_object()) {
        shape_error("AttributeError", "'str' object has no attribute 'get'");
    }
    if (!value.is_array()) {
        not_iterable(value);
    }
    return value;
}

// _int: an int that is not a bool, else 0.
std::int64_t py_int_field(const Json& value) {
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (value.is_number_unsigned()) {
        return static_cast<std::int64_t>(std::min<std::uint64_t>(
            value.get<std::uint64_t>(),
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
    }
    return 0;
}

Json effort_off() {
    return Json{{"off", "none"}};
}

// A hashable dict key: Python builds {e.get(key): e} and fails on a list or dict key.
std::map<std::string, Json> entries_by(const Json& list, std::string_view key) {
    std::map<std::string, Json> entries;
    for (const auto& entry : list_or_empty(list)) {
        const Json& name = dict_get(entry, key);
        if (name.is_array() || name.is_object()) {
            unhashable(name);
        }
        if (const auto* text = detail::text(name)) {
            entries[*text] = entry;
        }
    }
    return entries;
}

struct Answer {
    int status = 0;
    Json body; // the parsed JSON, else the text as a JSON string
};

http::Request get_request(std::string url, std::optional<std::string_view> api_key,
                          std::chrono::milliseconds timeout) {
    http::Request request;
    request.method = "GET";
    request.url = std::move(url);
    if (api_key && !api_key->empty()) {
        request.headers.emplace_back("Authorization", "Bearer " + std::string(*api_key));
    }
    request.connect_timeout = timeout;
    request.idle_timeout = timeout;
    request.follow_redirects = false;
    return request;
}

// What httpx raises for a failed transfer, with its class name.
CatalogError transport_error(const http::Error& error) {
    switch (error.kind) {
    case http::ErrorKind::Timeout:
        return {error.after_request ? "ReadTimeout" : "ConnectTimeout",
                error.message.empty() ? std::string("timed out") : error.message};
    case http::ErrorKind::Cancelled:
        return {"Cancelled", "canceled"};
    case http::ErrorKind::Dns:
    case http::ErrorKind::Connect:
    case http::ErrorKind::Tls:
        return {"ConnectError", error.message};
    default:
        return {error.after_head ? "ReadError" : "RemoteProtocolError", error.message};
    }
}

std::string httpx_status_message(const http::Response& response, const std::string& url) {
    const int status = response.status;
    std::string type;
    switch (status / 100) {
    case 1:
        type = "Informational response";
        break;
    case 3:
        type = "Redirect response";
        break;
    case 4:
        type = "Client error";
        break;
    case 5:
        type = "Server error";
        break;
    default:
        type = "Invalid status code";
        break;
    }
    std::string message =
        type + " '" + std::to_string(status) + " " + response.reason + "' for url '" + url + "'\n";
    if (status >= 300 && status < 400) {
        if (const auto location = http::find_header(response.headers, "location")) {
            message += "Redirect location: '" + *location + "'\n";
        }
    }
    return message +
           "For more information check: "
           "https://developer.mozilla.org/en-US/docs/Web/HTTP/Status/" +
           std::to_string(status);
}

// _json: the request, raise_for_status, then the body as JSON.
Json request_json(http::Client& client, http::Request request, const CancelToken& cancel) {
    const auto response = client.send(request, cancel);
    if (!response) {
        throw transport_error(response.error());
    }
    if (response->status < 200 || response->status >= 300) {
        throw CatalogError("HTTPStatusError", httpx_status_message(*response, request.url));
    }
    try {
        return py::loads(response->body);
    } catch (const py::PyError& error) {
        throw CatalogError("JSONDecodeError", error.what());
    } catch (const std::exception& error) {
        throw CatalogError("JSONDecodeError", error.what());
    }
}

// _probe: GET `path` as (status, body), or nullopt when the request fails. `api_key` is the key
// OpenWhisper's discovery client carries on every request, its identification probes included.
std::optional<Answer> probe(http::Client& client, const std::string& root, std::string_view path,
                            std::optional<std::string_view> api_key,
                            std::chrono::milliseconds timeout, const CancelToken& cancel) {
    const auto response =
        client.send(get_request(root + std::string(path), api_key, timeout), cancel);
    if (!response) {
        return std::nullopt;
    }
    Answer answer;
    answer.status = response->status;
    try {
        answer.body = py::loads(response->body);
    } catch (const std::exception&) {
        answer.body = response->body;
    }
    return answer;
}

using Get = std::function<std::optional<Answer>(std::string_view)>;

bool answers_ollama(const Get& get) {
    const auto answer = get("/");
    const auto* text = answer ? detail::text(answer->body) : nullptr;
    return text != nullptr && text->find("Ollama is running") != std::string::npos;
}

bool answers_llama_cpp(const Get& get) {
    const auto answer = get("/props");
    if (!answer || !answer->body.is_object()) {
        return false;
    }
    if (answer->body.contains("build_info")) {
        return true;
    }
    if (answer->status != 401) {
        return false;
    }
    const Json& error = detail::get(answer->body, "error");
    const Json& message = detail::truthy(error) ? dict_get(error, "message") : Json();
    return message == Json("Invalid API Key");
}

bool answers_vllm(const Get& get) {
    auto answer = get("/version");
    if (!(answer && answer->status == 200 && answer->body.is_object() &&
          answer->body.contains("version"))) {
        return false;
    }
    answer = get("/v1/models");
    bool vllm_owned = false;
    if (answer && answer->body.is_object()) {
        const Json& data = detail::get(answer->body, "data");
        // {row.get("owned_by") for row in data or [] if isinstance(row, dict)}: a string or a
        // mapping iterates as text, which the filter drops; anything else is not iterable, and an
        // owner that is a list or a mapping cannot go into the set.
        if (detail::truthy(data) && !(data.is_array() || data.is_object() || data.is_string())) {
            not_iterable(data);
        }
        if (data.is_array()) {
            for (const auto& row : data) {
                if (!row.is_object()) {
                    continue;
                }
                const Json& owner = detail::get(row, "owned_by");
                if (owner.is_array() || owner.is_object()) {
                    unhashable(owner);
                }
                if (owner == Json("vllm")) {
                    vllm_owned = true;
                }
            }
        }
    }
    // With --api-key the model list is closed but /version stays open.
    return (answer && answer->status == 401) || vllm_owned;
}

bool answers_lm_studio(const Get& get) {
    for (const std::string_view path : {"/api/v1/models", "/api/v0/models"}) {
        const auto answer = get(path);
        if (answer && answer->status == 200 && answer->body.is_object() &&
            (answer->body.contains("models") || answer->body.contains("data"))) {
            return true;
        }
    }
    return false;
}

std::optional<std::string>
server_from_owners(const std::vector<std::pair<std::string, Json>>& rows) {
    for (const auto& [id, row] : rows) {
        const Json& owner = detail::get(row, "owned_by");
        if (owner.is_array() || owner.is_object()) {
            unhashable(owner);
        }
        if (owner == Json("vllm")) {
            return "vllm";
        }
        if (owner == Json("llamacpp")) {
            return "llama_cpp";
        }
    }
    return std::nullopt;
}

std::optional<std::string> identify(http::Client& client, const std::string& root,
                                    const std::vector<std::pair<std::string, Json>>& rows,
                                    std::optional<std::string_view> api_key,
                                    std::chrono::milliseconds timeout, const CancelToken& cancel) {
    if (auto owner = server_from_owners(rows)) {
        return owner;
    }
    const Get get = [&](std::string_view path) {
        return probe(client, root, path, api_key, timeout, cancel);
    };
    const auto answer = get("/.well-known/localai.json");
    if (!answer) {
        return std::nullopt;
    }
    if (answer->status == 200 && answer->body.is_object() && answer->body.contains("endpoints")) {
        return "localai";
    }
    if (answers_ollama(get)) {
        return "ollama";
    }
    if (answers_llama_cpp(get)) {
        return "llama_cpp";
    }
    if (answers_vllm(get)) {
        return "vllm";
    }
    if (answers_lm_studio(get)) {
        return "lm_studio";
    }
    return std::nullopt;
}

using Specs = std::map<std::string, std::optional<TextModelSpec>>;

Specs describe_ollama(http::Client& client, const std::string& root,
                      const std::vector<std::string>& model_ids,
                      std::optional<std::string_view> api_key, std::chrono::milliseconds timeout,
                      const CancelToken& cancel) {
    // /api/show only reads installed metadata; it never pulls or loads a model.
    std::vector<std::optional<TextModelSpec>> results(model_ids.size());
    std::vector<std::exception_ptr> failures(model_ids.size());
    std::atomic<std::size_t> next{0};
    // executor.map cancels the calls not yet started once one fails: none starts after a failure.
    std::atomic<bool> failed{false};
    auto work = [&] {
        for (std::size_t index = next++; index < model_ids.size() && !failed; index = next++) {
            try {
                http::Request request = get_request(root + "/api/show", api_key, timeout);
                request.method = "POST";
                request.headers.emplace_back("Content-Type", "application/json");
                request.body = py::dumps(Json{{"model", model_ids[index]}});
                results[index] = describe_ollama_model(request_json(client, request, cancel));
            } catch (...) {
                failures[index] = std::current_exception();
                failed = true;
            }
        }
    };
    {
        std::vector<std::jthread> workers;
        const std::size_t count = std::min(kShowWorkers, model_ids.size());
        for (std::size_t i = 0; i < count; ++i) {
            workers.emplace_back(work);
        }
    }
    // executor.map raises the first failure in model order.
    for (const auto& failure : failures) {
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    Specs specs;
    for (std::size_t i = 0; i < model_ids.size(); ++i) {
        specs[model_ids[i]] = results[i];
    }
    return specs;
}

Specs describe(http::Client& client, const std::optional<std::string>& server,
               const std::string& root, const std::vector<std::string>& model_ids,
               const std::map<std::string, Json>& rows, std::optional<std::string_view> api_key,
               std::chrono::milliseconds timeout, const CancelToken& cancel) {
    Specs specs;
    if (server == "ollama") {
        return describe_ollama(client, root, model_ids, api_key, timeout, cancel);
    }
    if (server == "vllm") {
        for (const auto& model : model_ids) {
            specs[model] = describe_vllm_model(rows.at(model));
        }
        return specs;
    }
    if (server == "llama_cpp") {
        for (const auto& model : model_ids) {
            specs[model] = describe_llama_cpp_model(rows.at(model));
        }
        return specs;
    }
    if (server == "lm_studio") {
        Json listing;
        bool legacy = false;
        try {
            listing = request_json(client, get_request(root + "/api/v1/models", api_key, timeout),
                                   cancel);
            // The v1 answer is read inside OpenWhisper's try: a bad shape falls back to v0 too.
            entries_by(dict_get(listing, "models"), "key");
        } catch (const std::exception&) {
            listing = request_json(client, get_request(root + "/api/v0/models", api_key, timeout),
                                   cancel);
            legacy = true;
        }
        return describe_lm_studio(model_ids, listing, legacy);
    }
    if (server == "localai") {
        return describe_localai(
            model_ids,
            request_json(client, get_request(root + "/v1/models/capabilities", api_key, timeout),
                         cancel));
    }
    // An unidentified server: OpenWhisper's _DESCRIBE[None] raises KeyError.
    shape_error("KeyError", "None");
}

} // namespace

std::string server_root(std::string_view base_url) {
    std::string trimmed(base_url);
    while (!trimmed.empty() && trimmed.back() == '/') {
        trimmed.pop_back();
    }
    auto parts = detail::urlsplit(trimmed);
    if (parts.path.ends_with("/v1")) {
        parts.path.resize(parts.path.size() - 3);
    }
    parts.query.clear();
    parts.fragment.clear();
    return detail::urlunsplit(parts);
}

TextModelSpec server_spec(std::int64_t context, bool tools, bool reasoning,
                          const std::optional<Json>& effort) {
    TextModelSpec spec;
    if (context > 0) {
        context = std::max<std::int64_t>(1024, std::min<std::int64_t>(context, 2'000'000));
        spec.context_window = context;
        spec.max_output_tokens = std::min<std::int64_t>(4096, context / 2);
    }
    spec.tools = tools;
    spec.reasoning = reasoning;
    spec.reasoning_format = effort ? "openai" : "";
    if (effort && detail::truthy(*effort)) {
        spec.thinking_levels = *effort;
    }
    return spec;
}

std::optional<TextModelSpec> spec_by_name(std::string_view model) {
    static const py::Regex non_chat(R"(embed|rerank|whisper|(^|[-_/])tts([-_.]|$))",
                                    py::IGNORECASE);
    if (non_chat.search(model)) {
        return std::nullopt;
    }
    return server_spec();
}

std::optional<TextModelSpec> describe_ollama_model(const Json& show) {
    static const py::Regex num_ctx(R"((?m)^num_ctx\s+(\d+))");
    const Json& capabilities_value = dict_get(show, "capabilities");
    const Json capabilities =
        detail::truthy(capabilities_value) ? capabilities_value : Json::array();
    if (!py_in("completion", capabilities)) {
        return std::nullopt;
    }
    const Json& parameters = dict_get(show, "parameters");
    std::string text;
    if (detail::truthy(parameters)) {
        const auto* value = detail::text(parameters);
        if (value == nullptr) {
            shape_error("TypeError", "expected string or bytes-like object, got '" +
                                         std::string(detail::py_type_name(parameters)) + "'");
        }
        text = *value;
    }
    const bool thinking = py_in("thinking", capabilities);
    std::int64_t context = 4096;
    // Without num_ctx Ollama allocates 4096 tokens unless the GPU has 23 GiB or more, which this
    // client cannot see.
    if (const auto found = num_ctx.search(text)) {
        const auto digits = py::py_int(found->group(1));
        context = digits.value_or(std::numeric_limits<std::int64_t>::max());
    }
    return server_spec(context, py_in("tools", capabilities), thinking,
                       thinking ? std::optional<Json>(effort_off()) : std::nullopt);
}

std::optional<TextModelSpec> describe_vllm_model(const Json& row) {
    // Whether the model thinks is not reported; the effort field is accepted either way and only
    // reaches a template that reads it.
    return server_spec(py_int_field(dict_get(row, "max_model_len")), true, false, effort_off());
}

std::optional<TextModelSpec> describe_llama_cpp_model(const Json& row) {
    const Json& meta = dict_get(row, "meta");
    std::int64_t context = 0;
    if (detail::truthy(meta)) {
        context = py_int_field(dict_get(meta, "n_ctx"));
    }
    if (context == 0) {
        context = py_int_field(dict_get(row, "context_length"));
    }
    return server_spec(context, true, false, effort_off());
}

Specs describe_lm_studio(const std::vector<std::string>& model_ids, const Json& listing,
                         bool legacy) {
    const auto entries =
        entries_by(dict_get(listing, legacy ? "data" : "models"), legacy ? "id" : "key");
    Specs specs;
    for (const auto& model : model_ids) {
        const auto found = entries.find(model);
        if (found == entries.end()) {
            specs[model] = server_spec();
            continue;
        }
        const Json& entry = found->second;
        const Json& type = dict_get(entry, "type");
        if (type != Json("llm") && type != Json("vlm")) {
            specs[model] = std::nullopt;
            continue;
        }
        const Json& capabilities_value = dict_get(entry, "capabilities");
        if (legacy) {
            const bool tools =
                detail::truthy(capabilities_value) && py_in("tool_use", capabilities_value);
            specs[model] = server_spec(py_int_field(dict_get(entry, "max_context_length")), tools);
            continue;
        }
        const Json capabilities =
            detail::truthy(capabilities_value) ? capabilities_value : Json::object();
        std::int64_t context = 0;
        for (const auto& instance : list_or_empty(dict_get(entry, "loaded_instances"))) {
            const Json& config = dict_get(instance, "config");
            const std::int64_t loaded =
                detail::truthy(config) ? py_int_field(dict_get(config, "context_length")) : 0;
            if (loaded != 0 && context == 0) {
                context = loaded;
            }
        }
        if (context == 0) {
            context = py_int_field(dict_get(entry, "max_context_length"));
        }
        const Json& reasoning = dict_get(capabilities, "reasoning");
        std::set<std::string> options;
        bool any_option = false;
        if (detail::truthy(reasoning)) {
            const Json& allowed = dict_get(reasoning, "allowed_options");
            if (detail::truthy(allowed)) {
                if (allowed.is_array()) {
                    for (const auto& option : allowed) {
                        if (option.is_array() || option.is_object()) {
                            unhashable(option);
                        }
                        any_option = true;
                        if (const auto* text = detail::text(option)) {
                            options.insert(*text);
                        }
                    }
                } else if (const auto* text = detail::text(allowed)) {
                    // set() of a string is its characters.
                    any_option = true;
                    for (const char c : *text) {
                        options.insert(std::string(1, c));
                    }
                } else if (allowed.is_object()) {
                    for (const auto& [key, value] : allowed.items()) {
                        any_option = true;
                        options.insert(key);
                    }
                } else {
                    not_iterable(allowed);
                }
            }
        }
        std::optional<Json> effort;
        if (options.contains("low") || options.contains("medium") || options.contains("high")) {
            // Which effort values the OpenAI route takes beyond low/medium/high is undocumented,
            // so Off leaves the model's own default alone.
            Json levels = Json::object();
            levels["off"] = nullptr;
            for (const std::string_view level : {"low", "medium", "high"}) {
                levels[std::string(level)] =
                    options.contains(std::string(level)) ? Json(std::string(level)) : Json();
            }
            effort = std::move(levels);
        }
        const bool tools = dict_get(capabilities, "trained_for_tool_use") != Json(false);
        specs[model] = server_spec(context, tools, any_option, effort);
    }
    return specs;
}

Specs describe_localai(const std::vector<std::string>& model_ids, const Json& listing) {
    const auto entries = entries_by(dict_get(listing, "data"), "id");
    Specs specs;
    for (const auto& model : model_ids) {
        const auto found = entries.find(model);
        if (found == entries.end()) {
            specs[model] = spec_by_name(model);
            continue;
        }
        const Json& value = dict_get(found->second, "capabilities");
        const Json capabilities = detail::truthy(value) ? value : Json::array();
        if (!py_in("chat", capabilities)) {
            specs[model] = std::nullopt;
            continue;
        }
        const bool thinking = py_in("thinking", capabilities);
        specs[model] = server_spec(py_int_field(dict_get(found->second, "context_size")),
                                   py_in("tools", capabilities), thinking,
                                   thinking ? std::optional<Json>(effort_off()) : std::nullopt);
    }
    return specs;
}

std::vector<std::string> discover_chat_models(http::Client& client, std::string_view base_url,
                                              std::optional<std::string_view> server,
                                              std::optional<std::string_view> api_key,
                                              std::chrono::milliseconds timeout,
                                              const CancelToken& cancel) {
    const std::string root = server_root(base_url);
    std::string list_url(base_url);
    while (!list_url.empty() && list_url.back() == '/') {
        list_url.pop_back();
    }
    list_url += "/models";
    const Json listing = request_json(client, get_request(list_url, api_key, timeout), cancel);
    const Json& data = dict_get(listing, "data");
    // {row["id"]: row}: a repeated id keeps its first place and its last row.
    std::vector<std::pair<std::string, Json>> rows;
    std::map<std::string, Json> by_id;
    if (detail::truthy(data)) {
        if (!(data.is_array() || data.is_object() || data.is_string())) {
            not_iterable(data);
        }
        if (data.is_array()) {
            for (const auto& row : data) {
                const auto* id = row.is_object() ? detail::text(detail::get(row, "id")) : nullptr;
                if (id == nullptr) {
                    continue;
                }
                const auto existing =
                    std::ranges::find_if(rows, [&](const auto& pair) { return pair.first == *id; });
                if (existing == rows.end()) {
                    rows.emplace_back(*id, row);
                } else {
                    existing->second = row;
                }
                by_id[*id] = row;
            }
        }
    }
    std::vector<std::string> model_ids;
    for (const auto& [id, row] : rows) {
        model_ids.push_back(id);
    }
    std::optional<std::string> name =
        server ? std::optional<std::string>(std::string(*server)) : std::nullopt;
    if (!name) {
        name = identify(client, root, rows, api_key, timeout, cancel);
    }
    Specs specs;
    try {
        specs = describe(client, name, root, model_ids, by_id, api_key, timeout, cancel);
    } catch (const std::exception& error) {
        if (cancel.cancelled()) {
            throw CatalogError("Cancelled", "canceled");
        }
        const auto* python = dynamic_cast<const py::PyError*>(&error);
        kLog.debug("{} did not describe its models: {}", name.value_or("Server"),
                   python != nullptr ? std::string(python->type_name()) : std::string("Exception"));
        specs.clear();
        for (const auto& model : model_ids) {
            specs[model] = spec_by_name(model);
        }
    }
    std::map<std::string, TextModelSpec> known;
    std::vector<std::string> chat;
    for (const auto& [model, spec] : specs) {
        if (spec) {
            known[model] = *spec;
            chat.push_back(model);
        }
    }
    remember_server_specs(base_url, std::move(known));
    std::ranges::sort(chat);
    return chat;
}

std::optional<std::string> identify_server(http::Client& client, std::string_view base_url,
                                           std::chrono::milliseconds timeout,
                                           const CancelToken& cancel) {
    return identify(client, server_root(base_url), {}, std::nullopt, timeout, cancel);
}

std::vector<std::pair<std::string, std::string>>
detect_running_servers(http::Client& client, const std::vector<std::string>& base_urls,
                       Executor& executor, const CancelToken& cancel) {
    std::vector<std::string> candidates;
    for (const auto& url : base_urls) {
        if (is_loopback_url(url) && std::ranges::find(candidates, url) == candidates.end()) {
            candidates.push_back(url);
        }
    }
    if (candidates.empty()) {
        return {};
    }
    auto probe_one = [&client, cancel](const std::string& url) -> std::optional<std::string> {
        try {
            return identify_server(client, url, std::chrono::milliseconds(600), cancel);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };
    std::vector<std::optional<std::string>> found(candidates.size());
    if (executor.is_current()) {
        // Waiting here for probes posted to the executor this already runs on could wait for
        // itself (a strand one task wide): probe one address after another instead.
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            found[index] = probe_one(candidates[index]);
        }
    } else {
        struct Join {
            std::mutex mutex;
            std::condition_variable done;
            std::size_t remaining = 0;
            std::vector<std::optional<std::string>> found;
        };
        // Counts one probe down exactly once: when it has run, or when the executor destroys it
        // unrun (a pool or strand that was shut down drops what is posted), so the wait below
        // always ends and nothing is found at an address that was never probed.
        struct Ticket {
            Ticket(std::shared_ptr<Join> owner, std::size_t slot)
                : join(std::move(owner)), index(slot) {}
            Ticket(const Ticket&) = delete;
            Ticket& operator=(const Ticket&) = delete;
            ~Ticket() { finish(std::nullopt); }
            void finish(std::optional<std::string> server) {
                std::lock_guard lock(join->mutex);
                if (counted) {
                    return;
                }
                counted = true;
                join->found[index] = std::move(server);
                if (--join->remaining == 0) {
                    join->done.notify_all();
                }
            }
            std::shared_ptr<Join> join;
            std::size_t index;
            bool counted = false; // guarded by join->mutex
        };
        auto join = std::make_shared<Join>();
        join->remaining = candidates.size();
        join->found.resize(candidates.size());
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            auto ticket = std::make_shared<Ticket>(join, index);
            executor.post(
                [ticket, url = candidates[index], probe_one] { ticket->finish(probe_one(url)); });
        }
        std::unique_lock lock(join->mutex);
        join->done.wait(lock, [&] { return join->remaining == 0; });
        found = std::move(join->found);
    }
    std::vector<std::pair<std::string, std::string>> running;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (found[index]) {
            running.emplace_back(candidates[index], *found[index]);
        }
    }
    return running;
}

std::vector<std::string> discover_server_models(http::Client& client, const TextLLMProfile& profile,
                                                std::string_view key, KeySources* keys,
                                                std::chrono::milliseconds timeout,
                                                const CancelToken& cancel) {
    const auto resolved = detail::key_for(profile, key, keys);
    if (!resolved || resolved->empty()) {
        throw CatalogError("RuntimeError", "No API key found for " + profile.name + " (set " +
                                               profile.api_key_env + ")");
    }
    std::optional<std::string_view> server;
    if (is_local_server_id(profile.kind)) {
        server = profile.kind;
    }
    std::optional<std::string_view> sent;
    if (*resolved != kAuthFreeApiKey) {
        sent = *resolved;
    }
    return discover_chat_models(client, profile.base_url.value_or(""), server, sent, timeout,
                                cancel);
}

void learn_server_models(http::Client& client, const TextLLMProfile& profile, std::string_view key,
                         KeySources* keys, std::chrono::milliseconds timeout,
                         const CancelToken& cancel) {
    if (!profile.self_hosted()) {
        return;
    }
    if (server_specs_known(profile.base_url.value_or(""))) {
        return;
    }
    try {
        discover_server_models(client, profile, key, keys, timeout, cancel);
    } catch (const py::PyError& error) {
        kTextLlmLog.debug("Couldn't read {}'s model list: {}", profile.name, error.type_name());
    } catch (const std::exception&) {
        kTextLlmLog.debug("Couldn't read {}'s model list: {}", profile.name, "Exception");
    }
}

TextLLMSnapshot snapshot_from_profile(const TextLLMProfile& profile, std::string_view model,
                                      http::Client& client, KeySources& keys) {
    if (!model.empty()) {
        learn_server_models(client, profile, {}, &keys);
    }
    return snapshot_from_profile(profile, model);
}

Json resolve_meeting_llm_endpoint(const Json& settings, http::Client& client, KeySources& keys) {
    return snapshot_from_profile(resolve_meeting_llm_profile(settings),
                                 resolve_meeting_llm_model(settings), client, keys)
        .to_dict();
}

} // namespace fv::llm
