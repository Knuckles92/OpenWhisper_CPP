#include "agents/loopback_mcp.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <format>
#include <stdexcept>

#include "agents/detail/pyvalue.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/ids.h"
#include "base/log.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::agents {

namespace {

constexpr log::Logger logger("meeting.agent.installed.mcp_server");

constexpr std::array<std::string_view, 4> SUPPORTED_PROTOCOLS{"2025-11-25", "2025-06-18",
                                                              "2025-03-26", "2024-11-05"};
constexpr std::size_t MAX_BODY_BYTES = 4 * 1024 * 1024;
// Threads that answer requests off the reactor: tool calls block, one endpoint at a time.
constexpr int REQUEST_THREADS = 8;
// How long stop() lets requests in progress finish.
constexpr std::chrono::milliseconds STOP_GRACE{2000};

Json result(const Json& id, Json value) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(value)}};
}

Json error(const Json& id, int code, std::string_view message) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

Json tool_text(std::string_view text, bool is_error) {
    return Json{{"content", Json::array({Json{{"type", "text"}, {"text", text}}})},
                {"isError", is_error}};
}

// hmac.compare_digest: equal length and bytes, in time that does not depend on where they differ.
bool same_secret(std::string_view a, std::string_view b) {
    unsigned char differ = a.size() == b.size() ? 0 : 1;
    const auto& longer = a.size() >= b.size() ? a : b;
    for (std::size_t i = 0; i < longer.size(); ++i) {
        const auto x = static_cast<unsigned char>(i < a.size() ? a[i] : 0);
        const auto y = static_cast<unsigned char>(i < b.size() ? b[i] : 0);
        differ |= static_cast<unsigned char>(x ^ y);
    }
    return differ == 0;
}

McpReply json_reply(int status, const Json& body) {
    auto text = py::dumps(body);
    return McpReply{
        status,
        {{"Content-Type", "application/json"}, {"Content-Length", std::to_string(text.size())}},
        std::move(text)};
}

McpReply empty_reply(int status) {
    return McpReply{status, {{"Content-Length", "0"}}, {}};
}

// _refuse: the reply, and the connection closes after it.
McpReply refused(int status, std::string_view error_text) {
    auto reply = json_reply(status, Json{{"error", error_text}});
    reply.headers.emplace_back("Connection", "close");
    return reply;
}

// BaseHTTPRequestHandler.send_error(501, "Unsupported method (%r)"): the handler has no do_PUT
// (or do_HEAD, ...).
McpReply unsupported_method(std::string_view method) {
    const auto message = std::format("Unsupported method ({})", py::repr(method));
    const auto body =
        std::format("<!DOCTYPE HTML>\n"
                    "<html lang=\"en\">\n"
                    "    <head>\n"
                    "        <meta charset=\"utf-8\">\n"
                    "        <title>Error response</title>\n"
                    "    </head>\n"
                    "    <body>\n"
                    "        <h1>Error response</h1>\n"
                    "        <p>Error code: 501</p>\n"
                    "        <p>Message: {}.</p>\n"
                    "        <p>Error code explanation: 501 - {}.</p>\n"
                    "    </body>\n"
                    "</html>\n",
                    py::html_escape(message, false), "Server does not support this operation");
    return McpReply{501,
                    {{"Connection", "close"},
                     {"Content-Type", "text/html;charset=utf-8"},
                     {"Content-Length", std::to_string(body.size())}},
                    body};
}

// The first header of that name (email.message.Message.get).
std::optional<std::string_view> first_header(const net::Headers& headers, std::string_view name) {
    return net::header(headers, name);
}

} // namespace

std::string server_name() {
    return py::lower(identity().product);
}

// ---- Event ----

void Event::set() {
    {
        std::lock_guard lock(mutex_);
        set_ = true;
    }
    changed_.notify_all();
}

void Event::clear() {
    std::lock_guard lock(mutex_);
    set_ = false;
}

bool Event::is_set() const {
    std::lock_guard lock(mutex_);
    return set_;
}

bool Event::wait(std::chrono::duration<double> timeout) const {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return set_; });
}

// ---- McpEndpoint ----

McpEndpoint::McpEndpoint(std::string token_, Json tools_, ToolHandler handler_)
    : token(std::move(token_)), tools(tools_.is_array() ? std::move(tools_) : Json::array()),
      handler(std::move(handler_)) {}

std::vector<std::string> McpEndpoint::tool_names() const {
    std::vector<std::string> names;
    for (const auto& tool : tools) {
        names.push_back(detail::py_str(detail::get(tool, "name")));
    }
    return names;
}

// ---- LoopbackMcpServer ----

struct LoopbackMcpServer::Running {
    ThreadPool pool{"fv-mcp-loopback", REQUEST_THREADS};
    std::unique_ptr<net::HttpServer> http;
};

LoopbackMcpServer::LoopbackMcpServer(net::Reactor& reactor) : reactor_(reactor) {}

LoopbackMcpServer::~LoopbackMcpServer() {
    stop();
}

void LoopbackMcpServer::start() {
    std::lock_guard lock(mutex_);
    if (running_) {
        return;
    }
    auto running = std::make_unique<Running>();
    running->http = net::HttpServer::create(reactor_);
    const auto serve = [this](const net::HttpRequest& request,
                              std::shared_ptr<net::Responder> responder) {
        auto reply = answer(request);
        responder->send(reply.status, std::move(reply.headers), std::move(reply.body));
    };
    const net::RouteOptions options{.max_body = MAX_BODY_BYTES, .run_on = &running->pool};
    // One catch-all route per method the handler has, so the path is read here as Python reads
    // it (no slash redirects) and bodies up to the 4 MiB cap arrive whole.
    for (const auto* method : {"GET", "POST", "DELETE"}) {
        running->http->route(method, "/{rest:path}", options, serve);
    }
    running->http->set_fallback(
        [](const net::HttpRequest& request, bool, std::shared_ptr<net::Responder> responder) {
            auto reply = unsupported_method(request.method);
            responder->send(reply.status, std::move(reply.headers), std::move(reply.body));
        });
    const auto bound = running->http->listen(net::Listen{"127.0.0.1", 0, std::nullopt});
    if (!bound) {
        throw std::runtime_error("The tool server could not listen: " + bound.error());
    }
    port_ = bound->port;
    running_ = std::move(running);
    logger.info("Meeting tool server listening on 127.0.0.1:{}", port_);
}

void LoopbackMcpServer::stop() {
    std::unique_ptr<Running> running;
    {
        std::lock_guard lock(mutex_);
        running = std::move(running_);
        port_ = 0;
        // As Python's stop(): no wait for a call in progress, which may itself need the server.
        for (auto& [token, endpoint] : endpoints_) {
            endpoint->active = false;
        }
        endpoints_.clear();
    }
    if (running) {
        running->http->stop(STOP_GRACE);
        running->pool.shutdown(STOP_GRACE);
    }
}

bool LoopbackMcpServer::running() const {
    std::lock_guard lock(mutex_);
    return running_ != nullptr;
}

std::uint16_t LoopbackMcpServer::port() const {
    std::lock_guard lock(mutex_);
    return running_ ? port_ : 0;
}

std::shared_ptr<McpEndpoint> LoopbackMcpServer::open_endpoint(const Json& tools,
                                                              ToolHandler handler) {
    auto endpoint = std::make_shared<McpEndpoint>(token_urlsafe(24), tools, std::move(handler));
    std::lock_guard lock(mutex_);
    endpoints_.insert_or_assign(endpoint->token, endpoint);
    return endpoint;
}

void LoopbackMcpServer::close_endpoint(const std::shared_ptr<McpEndpoint>& endpoint) {
    if (!endpoint) {
        return;
    }
    {
        std::lock_guard calls(endpoint->lock);
        endpoint->active = false;
    }
    std::lock_guard lock(mutex_);
    endpoints_.erase(endpoint->token);
}

std::string LoopbackMcpServer::url(const McpEndpoint& endpoint) const {
    return std::format("http://127.0.0.1:{}/mcp/{}", port(), endpoint.token);
}

std::size_t LoopbackMcpServer::endpoint_count() const {
    std::lock_guard lock(mutex_);
    return endpoints_.size();
}

std::shared_ptr<McpEndpoint> LoopbackMcpServer::endpoint_for(const std::string& token) const {
    std::lock_guard lock(mutex_);
    const auto it = endpoints_.find(token);
    return it == endpoints_.end() ? nullptr : it->second;
}

std::optional<Json> LoopbackMcpServer::handle_message(McpEndpoint& endpoint, const Json& message) {
    if (!message.is_object()) {
        return error(nullptr, -32600, "invalid request");
    }
    const auto& method = detail::get(message, "method");
    const Json id = message.contains("id") ? message.at("id") : Json(nullptr);
    const auto& raw_params = detail::get(message, "params");
    const Json params = raw_params.is_object() ? raw_params : Json::object();
    if (!message.contains("id")) {
        return std::nullopt;
    }
    if (method == "initialize") {
        const auto& asked = detail::get(params, "protocolVersion");
        const auto requested = detail::truthy(asked) ? detail::py_str(asked) : std::string();
        std::string version(SUPPORTED_PROTOCOLS[1]);
        for (const auto supported : SUPPORTED_PROTOCOLS) {
            if (requested == supported) {
                version = requested;
            }
        }
        return result(id, Json{{"protocolVersion", version},
                               {"capabilities", {{"tools", {{"listChanged", false}}}}},
                               {"serverInfo",
                                {{"name", server_name()},
                                 {"title", std::string(identity().product) + " meeting tools"},
                                 {"version", "1"}}}});
    }
    if (method == "ping") {
        return result(id, Json::object());
    }
    if (method == "tools/list") {
        endpoint.listed.set();
        return result(id, Json{{"tools", endpoint.tools}});
    }
    if (method == "tools/call") {
        return result(id, call_tool(endpoint, params));
    }
    return error(id, -32601, "method not found: " + detail::py_str(method));
}

Json LoopbackMcpServer::call_tool(McpEndpoint& endpoint, const Json& params) {
    const auto& raw_name = detail::get(params, "name");
    const auto name = detail::truthy(raw_name) ? detail::py_str(raw_name) : std::string();
    const auto& raw_arguments = detail::get(params, "arguments");
    const Json arguments = raw_arguments.is_object() ? raw_arguments : Json::object();
    const auto& raw_meta = detail::get(params, "_meta");
    const Json meta = raw_meta.is_object() ? raw_meta : Json::object();
    std::lock_guard calls(endpoint.lock);
    if (!endpoint.active) {
        return tool_text("This meeting pass has ended; its tools are closed.", true);
    }
    const auto names = endpoint.tool_names();
    if (std::find(names.begin(), names.end(), name) == names.end()) {
        std::string listed;
        for (const auto& known : names) {
            listed += (listed.empty() ? "" : ", ") + known;
        }
        return tool_text("Unknown tool " + py::repr(name) + ". Use only: " + listed, true);
    }
    try {
        const auto [text, is_error] = endpoint.handler(name, arguments, meta);
        return tool_text(text, is_error);
    } catch (const std::exception& failure) {
        logger.warning("Meeting tool {} failed: {}", name, failure.what());
        return tool_text(py::dumps(Json{{"error", failure.what()}}), true);
    }
}

std::optional<std::shared_ptr<McpEndpoint>>
LoopbackMcpServer::authorized(const net::HttpRequest& request, McpReply& refusal) const {
    // path.split("?", 1)[0].strip("/").split("/")
    const auto parts = py::split_on(py::strip(request.path, "/"), "/");
    if (parts.size() != 2 || parts[0] != "mcp") {
        refusal = refused(404, "not found");
        return std::nullopt;
    }
    // Browsers send Origin; agents do not. A web page must never reach these tools, even from
    // this computer.
    const auto origin = first_header(request.headers, "Origin");
    if (origin && !origin->empty() && *origin != "null") {
        refusal = refused(403, "forbidden");
        return std::nullopt;
    }
    const auto& token = parts[1];
    const auto auth = first_header(request.headers, "Authorization").value_or("");
    auto endpoint = endpoint_for(token);
    if (!endpoint || !same_secret(auth, "Bearer " + token)) {
        refusal = refused(401, "unauthorized");
        return std::nullopt;
    }
    return endpoint;
}

McpReply LoopbackMcpServer::answer(const net::HttpRequest& request) {
    const auto& method = request.method;
    if (method != "GET" && method != "POST" && method != "DELETE") {
        return unsupported_method(method);
    }
    McpReply refusal;
    const auto endpoint = authorized(request, refusal);
    if (!endpoint) {
        return refusal;
    }
    if (method == "GET") {
        // No server-initiated stream: every reply rides its POST.
        return json_reply(405, Json{{"error", "no event stream"}});
    }
    if (method == "DELETE") {
        return json_reply(200, Json::object());
    }
    std::int64_t length = 0;
    if (const auto header = first_header(request.headers, "Content-Length");
        header && !header->empty()) {
        length = py::py_int(std::string(*header)).value_or(0);
    }
    if (length <= 0 || length > static_cast<std::int64_t>(MAX_BODY_BYTES) ||
        request.body_too_large) {
        return json_reply(length > 0 ? 413 : 400, Json{{"error", "bad body"}});
    }
    std::string_view bytes(request.body);
    bytes = bytes.substr(0, static_cast<std::size_t>(length));
    // json.loads of bytes: a UTF-8 byte order mark is skipped, and bytes that are not UTF-8 are a
    // parse error like bad JSON.
    if (bytes.starts_with("\xEF\xBB\xBF")) {
        bytes.remove_prefix(3);
    }
    Json message;
    try {
        if (!detail::is_valid_utf8(bytes)) {
            throw py::ValueError("not UTF-8");
        }
        message = py::loads(bytes);
    } catch (const py::ValueError&) {
        return json_reply(400, error(nullptr, -32700, "parse error"));
    }
    if (message.is_array()) {
        Json replies = Json::array();
        for (const auto& item : message) {
            if (auto reply = handle_message(**endpoint, item)) {
                replies.push_back(std::move(*reply));
            }
        }
        return replies.empty() ? empty_reply(202) : json_reply(200, replies);
    }
    auto reply = handle_message(**endpoint, message);
    return reply ? json_reply(200, *reply) : empty_reply(202);
}

} // namespace fv::agents
