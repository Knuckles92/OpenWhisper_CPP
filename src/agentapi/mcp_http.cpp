#include "agentapi/mcp_http.h"

#include <cctype>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "agentapi/detail/pydantic.h"
#include "agentapi/mcp_server.h"
#include "base/base64.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"

namespace fv::agentapi {

namespace {

constexpr std::string_view kEnvelopeModels =
    "union[JSONRPCRequest,JSONRPCNotification,JSONRPCResponse,JSONRPCError]";

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string strip(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t at = text.find(separator, start);
        out.emplace_back(text.substr(start, at == std::string_view::npos ? at : at - start));
        if (at == std::string_view::npos) {
            return out;
        }
        start = at + 1;
    }
}

std::string legacy_json(const Json& value) {
    return render_json(value); // model_dump_json: compact, non-ASCII kept
}

std::string modern_json(const Json& value) {
    py::DumpOptions options; // json.dumps(separators=(",", ":")): non-ASCII escaped
    options.item_sep = ",";
    options.key_sep = ":";
    return py::dumps(value, options);
}

ApiResponse plain(int status, std::string body, net::Headers headers = {}) {
    ApiResponse out;
    out.status = status;
    out.headers = std::move(headers);
    out.body = std::move(body);
    return out;
}

// StreamableHTTPServerTransport._create_error_response.
ApiResponse legacy_error(int status, int code, std::string_view message, net::Headers extra = {}) {
    Json error = Json::object();
    error["code"] = code;
    error["message"] = std::string(message);
    Json body = Json::object();
    body["jsonrpc"] = "2.0";
    body["id"] = nullptr;
    body["error"] = std::move(error);
    net::Headers headers{{"content-type", "application/json"}};
    for (auto& header : extra) {
        headers.push_back(std::move(header));
    }
    return plain(status, legacy_json(body), std::move(headers));
}

struct Accepts {
    bool json = false;
    bool sse = false;
};

// check_accept_headers, with RFC 7231 wildcards.
Accepts accepts(const net::HttpRequest& request) {
    Accepts out;
    const std::string header(net::header(request.headers, "accept").value_or(""));
    for (const std::string& item : split(header, ',')) {
        const std::string type = lower(strip(split(strip(item), ';')[0]));
        const bool any = type == "*/*";
        out.json = out.json || any || type == "application/json" || type == "application/*";
        out.sse = out.sse || any || type == "text/event-stream" || type == "text/*";
    }
    return out;
}

// TransportSecurityMiddleware.validate_request with DNS-rebinding protection on.
std::optional<ApiResponse> security(const net::HttpRequest& request,
                                    const std::vector<std::string>& allowed_hosts) {
    if (request.method == "POST") {
        const auto content_type = net::header(request.headers, "content-type");
        if (!content_type || !lower(*content_type).starts_with("application/json")) {
            return plain(400, "Invalid Content-Type header");
        }
    }
    const std::string host(net::header(request.headers, "host").value_or(""));
    bool allowed = false;
    if (!host.empty()) {
        for (const std::string_view base :
             {std::string_view("127.0.0.1"), std::string_view("localhost")}) {
            allowed = allowed || host == base || host.starts_with(std::string(base) + ":");
        }
        for (const std::string& base : allowed_hosts) {
            allowed = allowed || host == base || host.starts_with(base + ":");
        }
    }
    if (!allowed) {
        return plain(421, "Invalid Host header");
    }
    if (!net::header(request.headers, "origin").value_or("").empty()) {
        return plain(403, "Invalid Origin header");
    }
    return std::nullopt;
}

bool request_id(const Json& id) {
    return id.is_string() || id.is_number_integer() || id.is_number_unsigned();
}

bool params_shape(const Json& message) {
    return !message.contains("params") || message.at("params").is_object() ||
           message.at("params").is_null();
}

bool jsonrpc_ok(const Json& message) {
    return message.contains("jsonrpc") && message.at("jsonrpc").is_string() &&
           message.at("jsonrpc").get_ref<const std::string&>() == "2.0";
}

bool valid_request(const Json& message) {
    return message.is_object() && jsonrpc_ok(message) && message.contains("id") &&
           request_id(message.at("id")) && message.contains("method") &&
           message.at("method").is_string() && params_shape(message);
}

bool valid_notification(const Json& message) {
    return message.is_object() && jsonrpc_ok(message) && message.contains("method") &&
           message.at("method").is_string() && params_shape(message);
}

enum class Envelope { Request, Notification, Response, Error };

// jsonrpc_message_adapter.validate_python: the message's kind, or pydantic's union errors.
std::variant<Envelope, std::vector<detail::LineError>> classify_envelope(const Json& message) {
    std::vector<detail::LineError> errors;
    const char* models[] = {"JSONRPCRequest", "JSONRPCNotification", "JSONRPCResponse",
                            "JSONRPCError"};
    if (!message.is_object()) {
        for (const char* model : models) {
            errors.push_back(
                {model, "model_type",
                 std::string("Input should be a valid dictionary or instance of ") + model,
                 message});
        }
        return errors;
    }
    const auto field_errors = [&](const std::string& model, const std::vector<std::string>& fields,
                                  std::vector<detail::LineError>& out) {
        for (const std::string& field : fields) {
            const std::string loc = model + "." + field;
            if (!message.contains(field)) {
                if (field == "params") {
                    continue;
                }
                out.push_back({loc, "missing", "Field required", message});
                continue;
            }
            const Json& value = message.at(field);
            if (field == "jsonrpc") {
                if (!value.is_string() || value.get_ref<const std::string&>() != "2.0") {
                    out.push_back({loc, "literal_error", "Input should be '2.0'", value});
                }
            } else if (field == "id") {
                if (!request_id(value) && !(model == "JSONRPCError" && value.is_null())) {
                    out.push_back(
                        {loc + ".str", "string_type", "Input should be a valid string", value});
                    out.push_back(
                        {loc + ".int", "int_type", "Input should be a valid integer", value});
                }
            } else if (field == "method") {
                if (!value.is_string()) {
                    out.push_back({loc, "string_type", "Input should be a valid string", value});
                }
            } else if (field == "params" || field == "result") {
                if (!value.is_object() && !(field == "params" && value.is_null())) {
                    out.push_back({loc, "dict_type", "Input should be a valid dictionary", value});
                }
            } else if (field == "error") {
                if (!value.is_object()) {
                    out.push_back({loc, "model_type",
                                   "Input should be a valid dictionary or instance of ErrorData",
                                   value});
                } else {
                    if (!value.contains("code")) {
                        out.push_back({loc + ".code", "missing", "Field required", value});
                    } else if (!detail::lax_int(value.at("code"))) {
                        out.push_back({loc + ".code", "int_type", "Input should be a valid integer",
                                       value.at("code")});
                    }
                    if (!value.contains("message")) {
                        out.push_back({loc + ".message", "missing", "Field required", value});
                    } else if (!value.at("message").is_string()) {
                        out.push_back({loc + ".message", "string_type",
                                       "Input should be a valid string", value.at("message")});
                    }
                }
            }
        }
    };
    std::vector<detail::LineError> request, notification, response, error;
    field_errors(models[0], {"jsonrpc", "id", "method", "params"}, request);
    field_errors(models[1], {"jsonrpc", "method", "params"}, notification);
    field_errors(models[2], {"jsonrpc", "id", "result"}, response);
    field_errors(models[3], {"jsonrpc", "id", "error"}, error);
    if (request.empty()) {
        return Envelope::Request;
    }
    if (response.empty()) {
        return Envelope::Response;
    }
    if (error.empty()) {
        return Envelope::Error;
    }
    if (notification.empty()) {
        return Envelope::Notification;
    }
    for (auto* group : {&request, &notification, &response, &error}) {
        errors.insert(errors.end(), group->begin(), group->end());
    }
    return errors;
}

Json reply(const Json& id, const McpServer& server, const McpRequest& request,
           int* error_code = nullptr) {
    Json body = Json::object();
    body["jsonrpc"] = "2.0";
    body["id"] = id;
    try {
        body["result"] = server.dispatch(request);
    } catch (const RpcError& error) {
        body["error"] = error.to_json();
        if (error_code != nullptr) {
            *error_code = error.code();
        }
    } catch (const std::exception&) {
        // The SDK answers a handler's crash with a generic internal error.
        body["error"] = RpcError(INTERNAL_ERROR, "Internal error").to_json();
        if (error_code != nullptr) {
            *error_code = INTERNAL_ERROR;
        }
    }
    return body;
}

McpHttpAnswer legacy(const McpServer& server, const net::HttpRequest& request,
                     const std::vector<std::string>& allowed_hosts) {
    if (auto refused = security(request, allowed_hosts)) {
        return {std::move(*refused)};
    }
    if (request.method == "GET") {
        if (!accepts(request).sse) {
            return {legacy_error(406, INVALID_REQUEST,
                                 "Not Acceptable: Client must accept text/event-stream")};
        }
        McpHttpAnswer stream;
        stream.stream = true;
        stream.response.status = 200;
        stream.response.headers = {{"cache-control", "no-cache, no-transform"},
                                   {"content-type", "text/event-stream"},
                                   {"x-accel-buffering", "no"}};
        return stream;
    }
    if (request.method == "DELETE") {
        return {legacy_error(405, INVALID_REQUEST,
                             "Method Not Allowed: Session termination not supported")};
    }
    if (request.method != "POST") {
        return {legacy_error(405, INVALID_REQUEST, "Method Not Allowed",
                             {{"allow", "GET, POST, DELETE"}})};
    }
    if (!accepts(request).json) {
        return {legacy_error(406, INVALID_REQUEST,
                             "Not Acceptable: Client must accept application/json")};
    }
    const std::string content_type(net::header(request.headers, "content-type").value_or(""));
    bool json_type = false;
    for (const std::string& part : split(split(content_type, ';')[0], ',')) {
        json_type = json_type || strip(part) == "application/json";
    }
    if (!json_type) {
        return {legacy_error(415, INVALID_REQUEST,
                             "Unsupported Media Type: Content-Type must be application/json")};
    }
    auto parsed = detail::from_json(request.body);
    if (!parsed) {
        return {legacy_error(400, PARSE_ERROR, "Parse error: " + parsed.error())};
    }
    const Json& message = *parsed;
    auto kind = classify_envelope(message);
    if (auto* errors = std::get_if<std::vector<detail::LineError>>(&kind)) {
        return {legacy_error(400, INVALID_PARAMS,
                             "Validation error: " +
                                 detail::format_validation_error(kEnvelopeModels, *errors))};
    }
    if (std::get<Envelope>(kind) != Envelope::Request) {
        return {plain(202, "", {{"content-type", "application/json"}})};
    }
    McpRequest call;
    call.method = message.at("method").get<std::string>();
    call.params = message.contains("params") ? message.at("params") : Json(nullptr);
    call.id = message.at("id");
    call.modern = false;
    call.protocol_version =
        std::string(net::header(request.headers, "mcp-protocol-version").value_or(""));
    call.http = &request;
    const Json body = reply(call.id, server, call);
    return {plain(200, legacy_json(body), {{"content-type", "application/json"}})};
}

// ERROR_CODE_HTTP_STATUS
int modern_status(int code) {
    switch (code) {
    case PARSE_ERROR:
    case INVALID_REQUEST:
    case INVALID_PARAMS:
    case HEADER_MISMATCH:
    case MISSING_REQUIRED_CLIENT_CAPABILITY:
    case UNSUPPORTED_PROTOCOL_VERSION:
        return 400;
    case METHOD_NOT_FOUND:
        return 404;
    default:
        return 200;
    }
}

// _write of a JSONRPCError: with no id, "id": null comes last.
ApiResponse modern_error(const Json& id, const RpcError& error) {
    Json body = Json::object();
    body["jsonrpc"] = "2.0";
    if (!id.is_null()) {
        body["id"] = id;
    }
    body["error"] = error.to_json();
    if (id.is_null()) {
        body["id"] = nullptr;
    }
    return plain(modern_status(error.code()), modern_json(body),
                 {{"content-type", "application/json"}});
}

RpcError unsupported_version(std::string_view requested) {
    Json data = Json::object();
    data["supported"] = Json::array({std::string(MODERN_VERSION)});
    data["requested"] = std::string(requested);
    return RpcError(UNSUPPORTED_PROTOCOL_VERSION, "Unsupported protocol version", std::move(data));
}

bool valid_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        std::uint32_t point = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            length = 2;
            point = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            length = 3;
            point = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            length = 4;
            point = c & 0x07;
        } else {
            return false;
        }
        if (i + length > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            point = (point << 6) | (next & 0x3F);
        }
        const std::uint32_t minimum = length == 2 ? 0x80 : length == 3 ? 0x800 : 0x10000;
        if (point < minimum || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) {
            return false;
        }
        i += length;
    }
    return true;
}

// decode_header_value: the =?base64?...?= sentinel's UTF-8 text, nullopt for a malformed one.
std::optional<std::string> decode_header_value(std::optional<std::string_view> value) {
    if (!value) {
        return std::nullopt;
    }
    if (!value->starts_with("=?base64?") || !value->ends_with("?=") || value->size() < 11) {
        return std::string(*value);
    }
    const std::string_view payload = value->substr(9, value->size() - 11);
    const auto bytes = b64decode(payload);
    if (!bytes || b64encode(*bytes) != payload) {
        return std::nullopt;
    }
    std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    if (!valid_utf8(text)) {
        return std::nullopt;
    }
    return text;
}

// classify_inbound_request: the first rung the request fails, if any.
std::optional<RpcError> ladder(const Json& body, const net::HttpRequest& request) {
    const Json* meta = nullptr;
    if (body.contains("params") && body.at("params").is_object() &&
        body.at("params").contains("_meta")) {
        meta = &body.at("params").at("_meta");
    }
    if (meta == nullptr || !meta->is_object()) {
        return RpcError(INVALID_PARAMS, "params._meta must be an object carrying the required '" +
                                            std::string(PROTOCOL_VERSION_META_KEY) + "' and '" +
                                            std::string(CLIENT_CAPABILITIES_META_KEY) +
                                            "' envelope keys");
    }
    std::string missing;
    for (const std::string_view key : {PROTOCOL_VERSION_META_KEY, CLIENT_CAPABILITIES_META_KEY}) {
        if (!meta->contains(std::string(key))) {
            missing += (missing.empty() ? "" : ", ") + std::string(key);
        }
    }
    if (!missing.empty()) {
        return RpcError(INVALID_PARAMS,
                        "params._meta is missing the required envelope key(s): " + missing);
    }
    const Json& version = meta->at(std::string(PROTOCOL_VERSION_META_KEY));
    const auto version_header = net::header(request.headers, "mcp-protocol-version");
    if (!version_header || !version.is_string() ||
        *version_header != version.get_ref<const std::string&>()) {
        return RpcError(HEADER_MISMATCH, "mcp-protocol-version header does not match the "
                                         "request envelope's protocol version");
    }
    const std::string method = body.at("method").get<std::string>();
    const auto method_header = net::header(request.headers, "mcp-method");
    if (!method_header || *method_header != method) {
        return RpcError(HEADER_MISMATCH,
                        "mcp-method header does not match the request body's method");
    }
    std::string_view name_key;
    if (method == "tools/call" || method == "prompts/get") {
        name_key = "name";
    } else if (method == "resources/read") {
        name_key = "uri";
    }
    if (!name_key.empty()) {
        const Json& params = body.at("params");
        if (params.contains(std::string(name_key)) && !params.at(std::string(name_key)).is_null()) {
            const Json& value = params.at(std::string(name_key));
            const auto decoded = decode_header_value(net::header(request.headers, "mcp-name"));
            if (!decoded || !value.is_string() || *decoded != value.get_ref<const std::string&>()) {
                return RpcError(HEADER_MISMATCH, "mcp-name header does not match the request "
                                                 "body's '" +
                                                     std::string(name_key) + "' parameter");
            }
        }
    }
    if (version.get_ref<const std::string&>() != MODERN_VERSION) {
        return unsupported_version(version.get_ref<const std::string&>());
    }
    return std::nullopt;
}

McpHttpAnswer modern(const McpServer& server, const net::HttpRequest& request,
                     const std::vector<std::string>& allowed_hosts) {
    if (auto refused = security(request, allowed_hosts)) {
        return {std::move(*refused)};
    }
    if (request.method != "POST") {
        return {plain(405, "", {{"allow", "POST"}})};
    }
    if (!accepts(request).json) {
        return {plain(406, "")};
    }
    std::string_view text = request.body;
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3); // json.loads of bytes decodes UTF-8 with its signature
    }
    Json decoded;
    try {
        if (!valid_utf8(text)) {
            throw py::ValueError("not UTF-8");
        }
        decoded = py::loads(text);
    } catch (const py::PyError&) {
        return {modern_error(nullptr, RpcError(PARSE_ERROR, "Parse error"))};
    }
    const RpcError invalid_body(INVALID_REQUEST,
                                "Body must be a single JSON-RPC request or notification object");
    if (decoded.is_object() && !decoded.contains("id")) {
        if (!valid_notification(decoded)) {
            return {modern_error(nullptr, invalid_body)};
        }
        const std::string requested(
            net::header(request.headers, "mcp-protocol-version").value_or(""));
        if (requested != MODERN_VERSION) {
            return {modern_error(nullptr, unsupported_version(requested))};
        }
        return {plain(202, "")};
    }
    if (!valid_request(decoded)) {
        return {modern_error(nullptr, invalid_body)};
    }
    const Json& id = decoded.at("id");
    int seen[3] = {0, 0, 0};
    for (const auto& [name, value] : request.headers) {
        const std::string key = lower(name);
        const std::string_view names[] = {"mcp-protocol-version", "mcp-method", "mcp-name"};
        for (int i = 0; i < 3; ++i) {
            if (key == names[i] && ++seen[i] == 2) {
                return {modern_error(
                    id, RpcError(HEADER_MISMATCH, key + " header appears more than once"))};
            }
        }
    }
    if (auto rejection = ladder(decoded, request)) {
        return {modern_error(id, *rejection)};
    }
    McpRequest call;
    call.method = decoded.at("method").get<std::string>();
    call.params = decoded.contains("params") ? decoded.at("params") : Json(nullptr);
    call.id = id;
    call.modern = true;
    call.protocol_version = std::string(MODERN_VERSION);
    call.http = &request;
    int error_code = 0;
    const Json body = reply(id, server, call, &error_code);
    return {plain(error_code != 0 ? modern_status(error_code) : 200, modern_json(body),
                  {{"content-type", "application/json"}})};
}

// int(Content-Length) as Python reads it, or nullopt.
std::optional<std::int64_t> declared_length(const net::HttpRequest& request) {
    const auto header = net::header(request.headers, "content-length");
    if (!header) {
        return std::nullopt;
    }
    const auto value = py::py_int_checked(*header);
    if (!value) {
        return std::nullopt;
    }
    return *value;
}

} // namespace

McpHttpAnswer serve_mcp(const McpServer& server, const net::HttpRequest& request,
                        const std::vector<std::string>& allowed_hosts) {
    // RequestBodyLimitMiddleware, before anything reads the body.
    const auto declared = declared_length(request);
    if ((declared && *declared > static_cast<std::int64_t>(MCP_MAX_BODY)) ||
        request.body_too_large || request.body.size() > MCP_MAX_BODY) {
        return {plain(413, "Request body too large")};
    }
    const auto version = net::header(request.headers, "mcp-protocol-version");
    if (version && !is_handshake_version(*version)) {
        return modern(server, request, allowed_hosts);
    }
    return legacy(server, request, allowed_hosts);
}

} // namespace fv::agentapi
