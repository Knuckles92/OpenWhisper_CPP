#include "agentapi/mcp_server.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "agentapi/mcp_tools.h"

namespace fv::agentapi {

namespace {

[[noreturn]] void invalid_params() {
    // The SDK's answer to params its request model refuses, whatever the field.
    throw RpcError(INVALID_PARAMS, "Invalid request parameters", Json(""));
}

const Json& object_or_empty(const Json& params) {
    static const Json empty = Json::object();
    return params.is_object() ? params : empty;
}

// PaginatedRequestParams: an object (or nothing) whose cursor is a string or null.
void check_paginated(const Json& params) {
    if (params.is_null()) {
        return;
    }
    if (!params.is_object()) {
        invalid_params();
    }
    if (params.contains("cursor") && !params.at("cursor").is_string() &&
        !params.at("cursor").is_null()) {
        invalid_params();
    }
}

std::string required_string(const Json& params, const char* key) {
    if (!params.is_object() || !params.contains(key) || !params.at(key).is_string()) {
        invalid_params();
    }
    return params.at(key).get<std::string>();
}

Json text_content(std::string text) {
    Json item = Json::object();
    item["text"] = std::move(text);
    item["type"] = "text";
    Json content = Json::array();
    content.push_back(std::move(item));
    return content;
}

// The list results of the per-request wire, keys in the SDK's order.
Json modern_list(std::string_view key, Json items) {
    Json out = Json::object();
    out["cacheScope"] = "private";
    if (key < std::string_view("resultType")) {
        out[std::string(key)] = std::move(items);
        out["resultType"] = "complete";
    } else {
        out["resultType"] = "complete";
        out[std::string(key)] = std::move(items);
    }
    out["ttlMs"] = 0;
    return out;
}

// tools/list on the per-request wire: the same tools with inputSchema's "type" first and
// outputSchema's keys sorted, as the SDK dumps them there.
const Json& modern_tools() {
    static const Json tools = [] {
        Json out = Json::array();
        for (const Json& tool : mcp_catalog().tools) {
            Json copy = Json::object();
            for (const auto& [key, value] : tool.items()) {
                if (key == "inputSchema" && value.is_object() && value.contains("type")) {
                    Json schema = Json::object();
                    schema["type"] = value.at("type");
                    for (const auto& [name, item] : value.items()) {
                        if (name != "type") {
                            schema[name] = item;
                        }
                    }
                    copy[key] = std::move(schema);
                } else if (key == "outputSchema" && value.is_object()) {
                    std::vector<std::string> names;
                    for (const auto& [name, item] : value.items()) {
                        names.push_back(name);
                    }
                    std::sort(names.begin(), names.end());
                    Json schema = Json::object();
                    for (const std::string& name : names) {
                        schema[name] = value.at(name);
                    }
                    copy[key] = std::move(schema);
                } else {
                    copy[key] = value;
                }
            }
            out.push_back(std::move(copy));
        }
        return out;
    }();
    return tools;
}

} // namespace

bool is_handshake_version(std::string_view version) {
    for (const std::string_view known : HANDSHAKE_VERSIONS) {
        if (known == version) {
            return true;
        }
    }
    return false;
}

Json RpcError::to_json() const {
    Json out = Json::object();
    out["code"] = code_;
    out["message"] = what();
    if (data_) {
        out["data"] = *data_;
    }
    return out;
}

McpServer::McpServer(McpTools& tools) : tools_(tools) {}

void McpServer::register_method(std::string method, Handler handler) {
    std::lock_guard lock(mutex_);
    methods_[std::move(method)] = std::move(handler);
}

void McpServer::add_capability_hook(std::function<void(Json& capabilities)> hook) {
    std::lock_guard lock(mutex_);
    hooks_.push_back(std::move(hook));
}

Json McpServer::capabilities(Json base) const {
    std::vector<std::function<void(Json&)>> hooks;
    {
        std::lock_guard lock(mutex_);
        hooks = hooks_;
    }
    for (const auto& hook : hooks) {
        hook(base);
    }
    return base;
}

Json McpServer::initialize_result(std::string_view requested_version) const {
    Json listing = Json::object();
    listing["listChanged"] = false;
    Json resources = Json::object();
    resources["listChanged"] = false;
    resources["subscribe"] = false;
    Json base = Json::object();
    base["experimental"] = Json::object();
    base["prompts"] = listing;
    base["resources"] = std::move(resources);
    base["tools"] = listing;
    Json out = Json::object();
    out["capabilities"] = capabilities(std::move(base));
    out["instructions"] = tools_.instructions();
    out["protocolVersion"] = is_handshake_version(requested_version)
                                 ? std::string(requested_version)
                                 : std::string(LATEST_HANDSHAKE_VERSION);
    out["serverInfo"] = mcp_catalog().server;
    return out;
}

Json McpServer::discover_result() const {
    Json listing = Json::object();
    listing["listChanged"] = true;
    Json resources = Json::object();
    resources["listChanged"] = true;
    resources["subscribe"] = true;
    Json base = Json::object();
    base["prompts"] = listing;
    base["resources"] = std::move(resources);
    base["tools"] = listing;
    Json out = Json::object();
    out["cacheScope"] = "private";
    out["capabilities"] = capabilities(std::move(base));
    out["instructions"] = tools_.instructions();
    out["resultType"] = "complete";
    out["supportedVersions"] = Json::array({std::string(MODERN_VERSION)});
    out["ttlMs"] = 0;
    return out;
}

Json McpServer::stamp(Json result) const {
    if (!result.is_object()) {
        return result;
    }
    if (!result.contains("resultType")) {
        result["resultType"] = "complete";
    }
    Json server = Json::object();
    server[std::string(SERVER_INFO_META_KEY)] = mcp_catalog().server;
    if (!result.contains("_meta") || result.at("_meta").is_null()) {
        result["_meta"] = std::move(server);
    } else if (result.at("_meta").is_object() &&
               (!result.at("_meta").contains(std::string(SERVER_INFO_META_KEY)) ||
                result.at("_meta").at(std::string(SERVER_INFO_META_KEY)).is_null())) {
        result["_meta"][std::string(SERVER_INFO_META_KEY)] = mcp_catalog().server;
    }
    return result;
}

Json McpServer::call_tool(const McpRequest& request) const {
    const std::string name = required_string(request.params, "name");
    const Json& params = request.params;
    Json arguments = Json::object();
    if (params.contains("arguments") && !params.at("arguments").is_null()) {
        if (!params.at("arguments").is_object()) {
            invalid_params();
        }
        arguments = params.at("arguments");
    }
    const std::optional<ToolResult> found = tools_.call(name, arguments);
    Json out = Json::object();
    if (!found) {
        out["content"] = text_content("Unknown tool: " + name);
        out["isError"] = true;
    } else {
        out["content"] = text_content(found->text);
        out["isError"] = found->is_error;
    }
    if (request.modern) {
        out["resultType"] = "complete";
    }
    if (found && !found->is_error) {
        out["structuredContent"] = found->structured;
    }
    return out;
}

Json McpServer::legacy(const McpRequest& request) const {
    const std::string& method = request.method;
    if (method == "initialize") {
        const Json& params = request.params;
        if (!params.is_object() || !params.contains("protocolVersion") ||
            !params.at("protocolVersion").is_string() || !params.contains("capabilities") ||
            !params.at("capabilities").is_object() || !params.contains("clientInfo")) {
            invalid_params();
        }
        const Json& client = params.at("clientInfo");
        if (!client.is_object() || !client.contains("name") || !client.at("name").is_string() ||
            !client.contains("version") || !client.at("version").is_string()) {
            invalid_params();
        }
        return initialize_result(params.at("protocolVersion").get<std::string>());
    }
    if (method == "ping") {
        if (!request.params.is_null() && !request.params.is_object()) {
            invalid_params();
        }
        return Json::object();
    }
    if (method == "tools/list") {
        check_paginated(request.params);
        Json out = Json::object();
        out["tools"] = tools_.listed(mcp_catalog().tools);
        return out;
    }
    if (method == "tools/call") {
        return call_tool(request);
    }
    if (method == "resources/list" || method == "resources/templates/list" ||
        method == "prompts/list") {
        check_paginated(request.params);
        Json out = Json::object();
        out[method == "resources/list"             ? "resources"
            : method == "resources/templates/list" ? "resourceTemplates"
                                                   : "prompts"] = Json::array();
        return out;
    }
    if (method == "resources/read") {
        const std::string uri = required_string(request.params, "uri");
        Json data = Json::object();
        data["uri"] = uri;
        throw RpcError(INVALID_PARAMS, "Unknown resource: " + uri, std::move(data));
    }
    if (method == "prompts/get") {
        const std::string name = required_string(request.params, "name");
        throw RpcError(0, "Unknown prompt: " + name);
    }
    throw RpcError(METHOD_NOT_FOUND, "Method not found", Json(method));
}

Json McpServer::modern(const McpRequest& request) const {
    const std::string& method = request.method;
    const Json& params = object_or_empty(request.params);
    if (method == "server/discover") {
        return stamp(discover_result());
    }
    if (method == "tools/list") {
        check_paginated(params);
        return stamp(modern_list("tools", tools_.listed(modern_tools())));
    }
    if (method == "tools/call") {
        return stamp(call_tool(request));
    }
    if (method == "resources/list") {
        check_paginated(params);
        return stamp(modern_list("resources", Json::array()));
    }
    if (method == "resources/templates/list") {
        check_paginated(params);
        return stamp(modern_list("resourceTemplates", Json::array()));
    }
    if (method == "prompts/list") {
        check_paginated(params);
        return stamp(modern_list("prompts", Json::array()));
    }
    if (method == "resources/read" || method == "prompts/get") {
        return legacy(request);
    }
    throw RpcError(METHOD_NOT_FOUND, "Method not found", Json(method));
}

Json McpServer::dispatch(const McpRequest& request) const {
    Handler handler;
    {
        std::lock_guard lock(mutex_);
        if (const auto found = methods_.find(request.method); found != methods_.end()) {
            handler = found->second;
        }
    }
    if (handler) {
        Json result = handler(request);
        return request.modern ? stamp(std::move(result)) : result;
    }
    return request.modern ? modern(request) : legacy(request);
}

} // namespace fv::agentapi
