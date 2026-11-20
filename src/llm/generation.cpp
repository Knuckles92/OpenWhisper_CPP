#include "llm/generation.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <utility>

#include "llm/credentials.h"
#include "llm/detail/py_repr.h"
#include "llm/detail/truthy.h"
#include "llm/model_catalog.h"
#include "pyport/json.h"

namespace fv::llm {

namespace {

using detail::truthy;

constexpr std::array<std::string_view, 3> kUnfinishedResponseStatuses{"incomplete", "failed",
                                                                      "cancelled"};
// OpenCode profiles, whose requests carry provider_headers for the session.
constexpr std::array<std::string_view, 2> kSessionHeaderProfiles{"opencode_go", "opencode_zen"};

[[noreturn]] void attribute_error(const Json& value, std::string_view name) {
    throw py::PyError("AttributeError", "'" + std::string(detail::py_type_name(value)) +
                                            "' object has no attribute '" + std::string(name) +
                                            "'");
}

// mapping.get(key, default) as Python runs it on a parsed JSON value.
Json dict_get(const Json& mapping, std::string_view key, const Json& fallback = nullptr) {
    if (!mapping.is_object()) {
        attribute_error(mapping, "get");
    }
    const auto found = mapping.find(key);
    return found == mapping.end() ? fallback : *found;
}

// mapping[key].
const Json& item(const Json& mapping, std::string_view key) {
    if (!mapping.is_object()) {
        if (mapping.is_array()) {
            throw py::PyError("TypeError", "list indices must be integers or slices, not str");
        }
        throw py::PyError("TypeError", "'" + std::string(detail::py_type_name(mapping)) +
                                           "' object is not subscriptable");
    }
    const auto found = mapping.find(key);
    if (found == mapping.end()) {
        throw py::PyError("KeyError", detail::py_repr(Json(std::string(key))));
    }
    return *found;
}

bool contains(const Json& mapping, std::string_view key) {
    return mapping.is_object() && mapping.find(key) != mapping.end();
}

// "".join over values that must be str.
std::string join_texts(const std::vector<Json>& parts) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (!parts[i].is_string()) {
            throw py::PyError("TypeError",
                              "sequence item " + std::to_string(i) + ": expected str instance, " +
                                  std::string(detail::py_type_name(parts[i])) + " found");
        }
        out += parts[i].get_ref<const std::string&>();
    }
    return out;
}

std::string join_with(const std::vector<Json>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (!parts[i].is_string()) {
            throw py::PyError("TypeError",
                              "sequence item " + std::to_string(i) + ": expected str instance, " +
                                  std::string(detail::py_type_name(parts[i])) + " found");
        }
        if (i > 0) {
            out += separator;
        }
        out += parts[i].get_ref<const std::string&>();
    }
    return out;
}

// pydantic's model_dump(exclude_none=True) of a reply the SDK parsed into its models.
Json without_nulls(const Json& value) {
    if (value.is_object()) {
        Json out = Json::object();
        for (const auto& [key, entry] : value.items()) {
            if (!entry.is_null()) {
                out[key] = without_nulls(entry);
            }
        }
        return out;
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const auto& entry : value) {
            out.push_back(without_nulls(entry));
        }
        return out;
    }
    return value;
}

ToolCall make_call(const Json& id, const Json& name, const Json& arguments) {
    ToolCall call;
    call.id = id.is_string() ? id.get<std::string>() : detail::py_str(id);
    call.function.name = name.is_string() ? name.get<std::string>() : detail::py_str(name);
    call.function.arguments =
        arguments.is_string() ? arguments.get<std::string>() : py::dumps(arguments);
    return call;
}

// SimpleNamespace(prompt_tokens, completion_tokens, total_tokens) of a native protocol's usage.
Json native_usage(const Json& raw, bool google) {
    if (!truthy(raw)) {
        return nullptr;
    }
    Json incoming = dict_get(raw, google ? "promptTokenCount" : "input_tokens", 0);
    Json outgoing = dict_get(raw, google ? "candidatesTokenCount" : "output_tokens", 0);
    if (!truthy(incoming)) {
        incoming = 0;
    }
    if (!truthy(outgoing)) {
        outgoing = 0;
    }
    Json total;
    if ((incoming.is_number_integer() || incoming.is_boolean()) &&
        (outgoing.is_number_integer() || outgoing.is_boolean())) {
        const auto as_int = [](const Json& v) {
            return v.is_boolean() ? static_cast<std::int64_t>(v.get<bool>())
                                  : v.get<std::int64_t>();
        };
        total = as_int(incoming) + as_int(outgoing);
    } else if (incoming.is_number() && outgoing.is_number()) {
        total = incoming.get<double>() + outgoing.get<double>();
    } else {
        throw py::PyError("TypeError", "unsupported operand type(s) for +: '" +
                                           std::string(detail::py_type_name(incoming)) + "' and '" +
                                           std::string(detail::py_type_name(outgoing)) + "'");
    }
    Json usage = Json::object();
    usage["prompt_tokens"] = incoming;
    usage["completion_tokens"] = outgoing;
    usage["total_tokens"] = total;
    return usage;
}

Json assistant_message(const std::string& text, const std::vector<ToolCall>& calls) {
    Json result = Json::object();
    result["role"] = "assistant";
    result["content"] = text;
    if (!calls.empty()) {
        Json list = Json::array();
        for (const auto& call : calls) {
            Json function = Json::object();
            function["name"] = call.function.name;
            function["arguments"] = call.function.arguments;
            Json entry = Json::object();
            entry["id"] = call.id;
            entry["type"] = "function";
            entry["function"] = std::move(function);
            list.push_back(std::move(entry));
        }
        result["tool_calls"] = std::move(list);
    }
    return result;
}

void check_cancel(const CancelToken& cancel) {
    if (cancel.cancelled()) {
        throw TextGenerationError("canceled");
    }
}

// message.get("content") or "".
Json content_or_empty(const Json& message) {
    Json content = dict_get(message, "content");
    return truthy(content) ? content : Json("");
}

// The SDK's per-request options out of **options: timeout, extra_headers and extra_body.
struct SdkOptions {
    RequestOptions request;
    Json extra_body; // null when not given
};

SdkOptions take_sdk_options(Json& options) {
    SdkOptions out;
    if (const auto found = options.find("timeout"); found != options.end()) {
        if (found->is_number()) {
            out.request.timeout_s = found->get<double>();
        }
        options.erase(found);
    }
    if (const auto found = options.find("extra_headers"); found != options.end()) {
        if (found->is_object()) {
            for (const auto& [name, value] : found->items()) {
                out.request.headers.emplace_back(name, value.is_string() ? value.get<std::string>()
                                                                         : detail::py_str(value));
            }
        }
        options.erase(found);
    }
    if (const auto found = options.find("extra_body"); found != options.end()) {
        out.extra_body = *found;
        options.erase(found);
    }
    options.erase("extra_query");
    return out;
}

// The body an SDK resource method sends: its leading fields, then the other parameters in the
// order the SDK lists them (alphabetical), then extra_body merged over the top.
Json resource_body(Json leading, const Json& options, const Json& extra_body) {
    std::vector<std::string> names;
    for (const auto& [name, value] : options.items()) {
        if (!leading.contains(name)) {
            names.push_back(name);
        }
    }
    std::ranges::sort(names);
    for (const auto& name : names) {
        leading[name] = options.at(name);
    }
    if (extra_body.is_object()) {
        for (const auto& [name, value] : extra_body.items()) {
            leading[name] = value;
        }
    }
    return leading;
}

Json call_json(OpenAiCompatClient& client, std::string_view path, const Json& body,
               const RequestOptions& options, const CancelToken& cancel) {
    try {
        return client.post_json(path, body, options, cancel);
    } catch (const RequestCanceled&) {
        throw TextGenerationError("canceled");
    }
}

TextResult chat(OpenAiCompatClient& client, const std::string& model, const Json& messages,
                Json options, const CancelToken& cancel) {
    // Internal conversation metadata must never leak into another protocol.
    Json clean = Json::array();
    for (const auto& message : messages) {
        if (!message.is_object()) {
            attribute_error(message, "items");
        }
        Json kept = Json::object();
        for (const auto& [key, value] : message.items()) {
            if (!key.starts_with('_')) {
                kept[key] = value;
            }
        }
        clean.push_back(std::move(kept));
    }
    auto sdk = take_sdk_options(options);
    Json leading = Json::object();
    leading["messages"] = std::move(clean);
    leading["model"] = model;
    const Json body = resource_body(std::move(leading), options, sdk.extra_body);
    const Json response = call_json(client, "/chat/completions", body, sdk.request, cancel);

    const Json choices = response.is_object() ? response.value("choices", Json()) : Json();
    if (!choices.is_array() || choices.empty()) {
        return {};
    }
    const Json& choice = choices.front();
    const Json reason = choice.is_object() ? choice.value("finish_reason", Json("")) : Json("");
    if (reason.is_string() && (reason == "length" || reason == "content_filter")) {
        throw TextGenerationError("Model output was incomplete (" + reason.get<std::string>() +
                                  ").");
    }
    if (!contains(choice, "message")) {
        attribute_error(choice, "message");
    }
    const Json& message = choice.at("message");
    const Json refusal = message.is_object() ? message.value("refusal", Json()) : Json();
    if (refusal.is_string() && !refusal.get_ref<const std::string&>().empty()) {
        throw TextGenerationError("Model refused the request.");
    }
    Json content = message.is_object() ? message.value("content", Json()) : Json();
    if (!truthy(content)) {
        content = "";
    }
    if (!content.is_string()) {
        throw TextGenerationError("Model returned non-text content.");
    }
    std::vector<ToolCall> calls;
    const Json tool_calls = message.is_object() ? message.value("tool_calls", Json()) : Json();
    if (truthy(tool_calls) && tool_calls.is_array()) {
        for (const auto& entry : tool_calls) {
            const Json function = entry.is_object() ? entry.value("function", Json()) : Json();
            calls.push_back(
                make_call(entry.is_object() ? entry.value("id", Json()) : Json(),
                          function.is_object() ? function.value("name", Json()) : Json(),
                          function.is_object() ? function.value("arguments", Json()) : Json()));
        }
    }
    TextResult result;
    result.text = content.get<std::string>();
    result.assistant_message = assistant_message(result.text, calls);
    // DeepSeek, GLM and some gateways require these on subsequent tool turns. They are extra
    // fields of the SDK's message model: model_dump(exclude_none=True) drops a null one but
    // keeps nulls inside the value.
    for (const std::string_view key : {"reasoning_content", "reasoning", "reasoning_details"}) {
        if (contains(message, key) && !message.at(key).is_null()) {
            result.assistant_message[std::string(key)] = message.at(key);
        }
    }
    result.tool_calls = std::move(calls);
    const Json usage = response.value("usage", Json());
    result.usage = usage.is_null() ? Json() : without_nulls(usage);
    result.finish_reason = reason.is_string() ? reason.get<std::string>() : "";
    return result;
}

Json responses_input(const Json& messages) {
    Json items = Json::array();
    for (const auto& message : messages) {
        const Json& role = item(message, "role");
        if (role == "assistant" && contains(message, "_responses")) {
            for (const auto& entry : message.at("_responses")) {
                items.push_back(entry);
            }
        } else if (role == "tool") {
            Json output = Json::object();
            output["type"] = "function_call_output";
            output["call_id"] = item(message, "tool_call_id");
            output["output"] = item(message, "content");
            items.push_back(std::move(output));
        } else {
            Json entry = Json::object();
            entry["role"] = role;
            entry["content"] = content_or_empty(message);
            items.push_back(std::move(entry));
        }
    }
    return items;
}

std::vector<Json> responses_message_texts(const Json& entry) {
    std::vector<Json> texts;
    const Json parts = dict_get(entry, "content", Json::array());
    for (const auto& part : parts) {
        const Json type = dict_get(part, "type");
        if (type == "refusal") {
            throw TextGenerationError("Model refused the request.");
        }
        if (type == "output_text") {
            texts.push_back(item(part, "text"));
        }
    }
    return texts;
}

Json function_schemas(const Json& tools) {
    Json out = Json::array();
    for (const auto& tool : tools) {
        Json entry = Json::object();
        entry["type"] = "function";
        entry["strict"] = false;
        for (const auto& [key, value] : item(tool, "function").items()) {
            entry[key] = value;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

TextResult responses(OpenAiCompatClient& client, const std::string& model, const Json& messages,
                     const std::optional<Json>& tools, const Json& limit, bool json_mode,
                     Json options, const CancelToken& cancel) {
    Json leading = Json::object();
    leading["input"] = responses_input(messages);
    leading["model"] = model;
    leading["store"] = false;
    leading["max_output_tokens"] = limit;
    if (tools && truthy(*tools)) {
        // Preserve optional parameters from our Chat Completions tool schemas.
        leading["tools"] = function_schemas(*tools);
        leading["tool_choice"] = "auto";
    }
    if (json_mode) {
        leading["text"] = Json{{"format", Json{{"type", "json_object"}}}};
    }
    auto sdk = take_sdk_options(options);
    const Json body = resource_body(std::move(leading), options, sdk.extra_body);
    const Json response = call_json(client, "/responses", body, sdk.request, cancel);
    const Json raw = response.is_object() ? without_nulls(response) : Json::object();

    const Json status = dict_get(raw, "status");
    if ((status.is_string() &&
         std::ranges::find(kUnfinishedResponseStatuses, status.get_ref<const std::string&>()) !=
             kUnfinishedResponseStatuses.end()) ||
        truthy(dict_get(raw, "error"))) {
        throw TextGenerationError("Model output was incomplete or failed.");
    }
    Json output = dict_get(raw, "output");
    if (!truthy(output)) {
        output = Json::array();
    }
    std::vector<Json> texts;
    std::vector<ToolCall> calls;
    for (const auto& entry : output) {
        const Json type = dict_get(entry, "type");
        if (type == "function_call") {
            calls.push_back(
                make_call(item(entry, "call_id"), item(entry, "name"), item(entry, "arguments")));
        } else if (type == "message") {
            for (auto& text : responses_message_texts(entry)) {
                texts.push_back(std::move(text));
            }
        }
    }
    TextResult result;
    result.text = join_texts(texts);
    result.assistant_message = assistant_message(result.text, calls);
    result.assistant_message["_responses"] = output;
    result.tool_calls = std::move(calls);
    Json usage = dict_get(raw, "usage");
    result.usage = native_usage(truthy(usage) ? usage : Json::object(), false);
    const Json finish = dict_get(raw, "status", "");
    result.finish_reason = finish.is_string() ? finish.get<std::string>() : "";
    return result;
}

// A dict-typed client.post: the SDK passes timeout and headers; the other options are its own.
RequestOptions native_request(Json& options) {
    auto sdk = take_sdk_options(options);
    return sdk.request;
}

TextResult anthropic(OpenAiCompatClient& client, const std::string& model, const Json& messages,
                     const std::optional<Json>& tools, const Json& limit, Json options,
                     const CancelToken& cancel) {
    std::vector<Json> system;
    Json conversation = Json::array();
    for (const auto& message : messages) {
        std::string role = detail::py_str(item(message, "role"));
        if (role == "system" || role == "developer") {
            system.push_back(content_or_empty(message));
            continue;
        }
        Json content;
        if (role == "assistant") {
            content = dict_get(message, "_anthropic");
            if (content.is_null()) {
                content =
                    Json::array({Json{{"type", "text"}, {"text", content_or_empty(message)}}});
            }
        } else if (role == "tool") {
            role = "user";
            content = Json::array({Json{{"type", "tool_result"},
                                        {"tool_use_id", item(message, "tool_call_id")},
                                        {"content", item(message, "content")}}});
        } else {
            content = Json::array({Json{{"type", "text"}, {"text", content_or_empty(message)}}});
        }
        if (!conversation.empty() && conversation.back().at("role") == role) {
            for (const auto& part : content) {
                conversation.back()["content"].push_back(part);
            }
        } else {
            Json turn = Json::object();
            turn["role"] = role;
            turn["content"] = Json::array();
            for (const auto& part : content) {
                turn["content"].push_back(part);
            }
            conversation.push_back(std::move(turn));
        }
    }
    Json body = Json::object();
    body["model"] = model;
    body["system"] = join_with(system, "\n\n");
    body["messages"] = std::move(conversation);
    body["max_tokens"] = limit;
    if (tools && truthy(*tools)) {
        Json list = Json::array();
        for (const auto& tool : *tools) {
            const Json& function = item(tool, "function");
            Json entry = Json::object();
            entry["name"] = item(function, "name");
            entry["description"] = dict_get(function, "description", "");
            entry["input_schema"] = item(function, "parameters");
            list.push_back(std::move(entry));
        }
        body["tools"] = std::move(list);
        body["tool_choice"] = Json{{"type", "auto"}};
    }
    // JSON fallback instructions live in the shared meeting prompt. Do not require
    // structured-output extensions unsupported by some Messages models.
    RequestOptions request = native_request(options);
    request.headers.emplace_back("anthropic-version", "2023-06-01");
    const Json raw = call_json(client, "/messages", body, request, cancel);

    const Json reason = dict_get(raw, "stop_reason", "");
    if (reason == "max_tokens" || reason == "refusal" || reason == "pause_turn") {
        throw TextGenerationError("Model output was incomplete (" + reason.get<std::string>() +
                                  ").");
    }
    Json content = dict_get(raw, "content");
    if (!truthy(content)) {
        content = Json::array();
    }
    std::vector<Json> texts;
    std::vector<ToolCall> calls;
    for (const auto& part : content) {
        if (dict_get(part, "type") == "text") {
            texts.push_back(dict_get(part, "text", ""));
        }
    }
    for (const auto& part : content) {
        if (dict_get(part, "type") == "tool_use") {
            calls.push_back(make_call(item(part, "id"), item(part, "name"), item(part, "input")));
        }
    }
    TextResult result;
    result.text = join_texts(texts);
    result.assistant_message = assistant_message(result.text, calls);
    result.assistant_message["_anthropic"] = content;
    result.tool_calls = std::move(calls);
    Json usage = dict_get(raw, "usage");
    result.usage = native_usage(truthy(usage) ? usage : Json::object(), false);
    result.finish_reason = reason.is_string() ? reason.get<std::string>() : "";
    return result;
}

// urllib.parse.quote(text, safe=''): everything but letters, digits and "_.-~" percent-encoded.
std::string quote_all(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '_' || byte == '.' || byte == '-' ||
            byte == '~') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0F]);
        }
    }
    return out;
}

// _google_turn: the Gemini role and parts of one non-system message.
std::pair<std::string, Json> google_turn(const Json& message, const std::string& role,
                                         std::map<std::string, Json>& call_names) {
    if (role == "assistant") {
        // Tool results name their function, so remember each call's name.
        for (const auto& call : dict_get(message, "tool_calls", Json::array())) {
            call_names[detail::py_str(item(call, "id"))] = item(item(call, "function"), "name");
        }
        Json parts = dict_get(message, "_google");
        if (!truthy(parts)) {
            parts = Json::array({Json{{"text", content_or_empty(message)}}});
        }
        return {"model", parts};
    }
    if (role == "tool") {
        const Json& call_id = item(message, "tool_call_id");
        const auto found = call_names.find(detail::py_str(call_id));
        if (found == call_names.end()) {
            throw py::PyError("KeyError", detail::py_repr(call_id));
        }
        Json response = Json::object();
        response["id"] = call_id;
        response["name"] = found->second;
        response["response"] = Json{{"result", item(message, "content")}};
        return {"user", Json::array({Json{{"functionResponse", std::move(response)}}})};
    }
    return {role, Json::array({Json{{"text", content_or_empty(message)}}})};
}

TextResult google(OpenAiCompatClient& client, const std::string& model, const Json& messages,
                  const std::optional<Json>& tools, const Json& limit, bool json_mode, Json options,
                  const CancelToken& cancel) {
    std::vector<Json> system;
    Json contents = Json::array();
    std::map<std::string, Json> call_names;
    for (const auto& message : messages) {
        const std::string role = detail::py_str(item(message, "role"));
        if (role == "system" || role == "developer") {
            system.push_back(content_or_empty(message));
            continue;
        }
        auto [turn_role, parts] = google_turn(message, role, call_names);
        if (!contents.empty() && contents.back().at("role") == turn_role) {
            for (const auto& part : parts) {
                contents.back()["parts"].push_back(part);
            }
        } else {
            Json turn = Json::object();
            turn["role"] = turn_role;
            turn["parts"] = Json::array();
            for (const auto& part : parts) {
                turn["parts"].push_back(part);
            }
            contents.push_back(std::move(turn));
        }
    }
    Json body = Json::object();
    body["contents"] = std::move(contents);
    body["generationConfig"] = Json{{"maxOutputTokens", limit}};
    if (!system.empty()) {
        body["systemInstruction"] =
            Json{{"parts", Json::array({Json{{"text", join_with(system, "\n\n")}}})}};
    }
    if (tools && truthy(*tools)) {
        Json declarations = Json::array();
        for (const auto& tool : *tools) {
            const Json& function = item(tool, "function");
            Json entry = Json::object();
            entry["name"] = item(function, "name");
            entry["description"] = dict_get(function, "description", "");
            entry["parametersJsonSchema"] = item(function, "parameters");
            declarations.push_back(std::move(entry));
        }
        body["tools"] = Json::array({Json{{"functionDeclarations", std::move(declarations)}}});
    }
    if (json_mode) {
        body["generationConfig"]["responseMimeType"] = "application/json";
    }
    const RequestOptions request = native_request(options);
    const Json raw = call_json(client, "/models/" + quote_all(model) + ":generateContent", body,
                               request, cancel);

    Json candidates = dict_get(raw, "candidates");
    if (!truthy(candidates)) {
        throw TextGenerationError("Model returned no candidate (possibly blocked).");
    }
    const Json& candidate = candidates.front();
    const Json reason = dict_get(candidate, "finishReason", "");
    if (!(reason == "STOP" || reason == "")) {
        throw TextGenerationError("Model output was incomplete (" + detail::py_str(reason) + ").");
    }
    Json parts = dict_get(dict_get(candidate, "content", Json::object()), "parts");
    if (!truthy(parts)) {
        parts = Json::array();
    }
    std::vector<Json> texts;
    for (const auto& part : parts) {
        if (!truthy(dict_get(part, "thought", false))) {
            texts.push_back(dict_get(part, "text", ""));
        }
    }
    std::vector<ToolCall> calls;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const Json call = dict_get(parts[index], "functionCall");
        if (truthy(call)) {
            Json id = dict_get(call, "id");
            if (!truthy(id)) {
                id = "call_" + std::to_string(index);
            }
            calls.push_back(
                make_call(id, item(call, "name"), dict_get(call, "args", Json::object())));
        }
    }
    TextResult result;
    result.text = join_texts(texts);
    result.assistant_message = assistant_message(result.text, calls);
    result.assistant_message["_google"] = parts;
    result.tool_calls = std::move(calls);
    Json usage = dict_get(raw, "usageMetadata");
    result.usage = native_usage(truthy(usage) ? usage : Json::object(), true);
    result.finish_reason = reason.is_string() ? reason.get<std::string>() : "";
    return result;
}

void set_chat_options(const TextModelSpec& spec, const Json& level, bool hosted,
                      const std::optional<Json>& tools, Json& options) {
    const bool thinking = truthy(level) && level != "off";
    if (thinking && spec.reasoning_format == "openai") {
        options["reasoning_effort"] = level;
        if (level != "none") {
            // Thinking models are not meant to sample greedily.
            options.erase("temperature");
        }
    } else if (thinking && spec.reasoning_format == "deepseek") {
        options["extra_body"] = Json{{"thinking", Json{{"type", "enabled"}}}};
        if (level == "high" || level == "max") {
            options["extra_body"]["reasoning_effort"] = level;
        }
    }
    if (hosted && !options.contains("max_tokens")) {
        options["max_tokens"] = spec.max_output_tokens;
    }
    if (tools && truthy(*tools)) {
        options["tools"] = *tools;
    }
}

// _set_native_options: strips Chat Completions options; returns the output limit and JSON mode.
std::pair<Json, bool> set_native_options(const TextModelSpec& spec, const Json& level,
                                         Json& options) {
    Json limit = spec.max_output_tokens;
    if (const auto found = options.find("max_tokens"); found != options.end()) {
        limit = *found;
        options.erase(found);
    }
    bool json_mode = false;
    if (const auto found = options.find("response_format"); found != options.end()) {
        json_mode = !found->is_null();
        options.erase(found);
    }
    options.erase("tool_choice");
    if (spec.protocol != "responses" || spec.reasoning) {
        options.erase("temperature");
    }
    options.erase("reasoning_effort");
    options.erase("extra_body");
    if (truthy(level) && level != "off" && spec.reasoning_format == "openai") {
        options["reasoning"] = Json{{"effort", level}};
    }
    if (spec.protocol == "responses" && spec.reasoning_format == "openai") {
        options["include"] = Json::array({"reasoning.encrypted_content"});
    }
    return {limit, json_mode};
}

bool is_hosted_catalog(const TextLLMProfile* profile) {
    return profile != nullptr && std::ranges::find(kHostedCatalogProfileIds, profile->kind) !=
                                     kHostedCatalogProfileIds.end();
}

} // namespace

TextResult generate(OpenAiCompatClient& client, const TextLLMProfile& profile,
                    GenerateRequest request) {
    return generate(client, &profile, std::move(request));
}

TextResult generate(OpenAiCompatClient& client, const TextLLMProfile* profile,
                    GenerateRequest request) {
    check_cancel(request.cancel);
    const TextModelSpec spec =
        profile != nullptr ? model_spec(*profile, request.model) : TextModelSpec{};
    if (request.tools && truthy(*request.tools) && !spec.tools) {
        throw TextGenerationError("This model does not support tool calls.");
    }
    Json options = request.options.is_object() ? std::move(request.options) : Json::object();
    if (!request.session_id.empty() && profile != nullptr &&
        std::ranges::find(kSessionHeaderProfiles, profile->kind) != kSessionHeaderProfiles.end()) {
        Json headers = Json::object();
        if (const auto provided = provider_headers(*profile, request.session_id)) {
            for (const auto& [name, value] : *provided) {
                headers[name] = value;
            }
        }
        options["extra_headers"] = std::move(headers);
    }
    // A reviewed hosted catalog decides sampling, reasoning and output budget. Servers the user
    // runs and custom endpoints keep the caller's options: their limits are the server's, and
    // an unknown output limit is left to it.
    const bool hosted = is_hosted_catalog(profile);
    if (hosted) {
        options.erase("temperature");
        options.erase("reasoning_effort");
        options.erase("extra_body");
    }
    Json level = request.reasoning_level;
    if (spec.thinking_levels && spec.thinking_levels->is_object()) {
        if (const auto found = spec.thinking_levels->find(request.reasoning_level);
            found != spec.thinking_levels->end()) {
            level = *found;
        }
    }
    TextResult result;
    if (spec.protocol == "chat") {
        set_chat_options(spec, level, hosted, request.tools, options);
        result = chat(client, request.model, request.messages, std::move(options), request.cancel);
    } else {
        auto [limit, json_mode] = set_native_options(spec, level, options);
        if (spec.protocol == "responses") {
            result = responses(client, request.model, request.messages, request.tools, limit,
                               json_mode, std::move(options), request.cancel);
        } else if (spec.protocol == "anthropic") {
            result = anthropic(client, request.model, request.messages, request.tools, limit,
                               std::move(options), request.cancel);
        } else if (spec.protocol == "google") {
            result = google(client, request.model, request.messages, request.tools, limit,
                            json_mode, std::move(options), request.cancel);
        } else {
            throw py::PyError("KeyError", detail::py_repr(Json(spec.protocol)));
        }
    }
    check_cancel(request.cancel);
    return result;
}

} // namespace fv::llm
