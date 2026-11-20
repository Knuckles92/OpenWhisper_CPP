#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "net/http_server.h"

// The app's tools as a loopback MCP server, streamable HTTP (meeting/agent/installed/
// mcp_server.py). An installed agent reaches its tools here and nowhere else. The server binds
// 127.0.0.1 on a random port and answers `POST /mcp/<token>` with a JSON-RPC reply (no event
// stream, which every MCP client accepts). The token names one endpoint: a pass, or a router
// that picks a pass per call. It must match the `Authorization: Bearer` header too. An endpoint
// that has closed answers every call with an error, so a finished or canceled pass has no
// authority left. Tool calls within an endpoint run one at a time, so a batch lands in the order
// the agent sent it.
namespace fv::agents {

// handler(tool_name, arguments, meta) -> (text, is_error). Arguments and meta are JSON objects.
using ToolHandler = std::function<std::pair<std::string, bool>(std::string_view name,
                                                               const Json& args, const Json& meta)>;

// The MCP server's name: identity().product in lower case ("fiorivoce"). Claude Code names our
// tools mcp__<name>__<tool>, OpenCode <name>_<tool>.
std::string server_name();

// A flag threads wait on (threading.Event).
// Affinity: thread-safe.
class Event {
public:
    void set();
    void clear();
    bool is_set() const;
    // True when set before `timeout` passed.
    bool wait(std::chrono::duration<double> timeout) const;

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    bool set_ = false;
};

// One token's view of the server: which tools it lists and who answers.
// Affinity: thread-safe; the handler's calls run one at a time under `lock`.
struct McpEndpoint {
    McpEndpoint(std::string token, Json tools, ToolHandler handler);

    const std::string token; // path and bearer secret
    const Json tools;        // MCP tool definitions tools/list returns (a JSON array)
    const ToolHandler handler;
    // Held while a tool call runs; close_endpoint takes it, so it waits for that call.
    std::mutex lock;
    // False once closed; every later call is refused. A call reads it under `lock`.
    std::atomic<bool> active{true};
    // Set by the first tools/list: the agent has seen the tools.
    Event listed;

    // str(tool.get("name")) of each tool.
    std::vector<std::string> tool_names() const;
};

// One HTTP answer of the server, before it goes on the wire.
struct McpReply {
    int status = 200;
    net::Headers headers;
    std::string body;
};

// The tool server of one meeting, report or one-off task. start() it, open an endpoint per pass,
// close it when the pass ends, and stop() it when done.
// Affinity: thread-safe; stop() and the destructor are not called from the reactor's threads or
// from inside a tool handler.
class LoopbackMcpServer {
public:
    explicit LoopbackMcpServer(net::Reactor& reactor);
    ~LoopbackMcpServer(); // stop()
    LoopbackMcpServer(const LoopbackMcpServer&) = delete;
    LoopbackMcpServer& operator=(const LoopbackMcpServer&) = delete;

    // Listens on 127.0.0.1 with a port the system picks; a second call does nothing. Throws
    // std::runtime_error when it cannot listen.
    void start();
    // Closes every endpoint and stops listening. Requests in progress get 2 s; a tool call still
    // running after that is waited for, since its handler answers through this server.
    void stop();
    bool running() const;
    std::uint16_t port() const; // 0 when not running

    // Registers a new endpoint with a fresh token (secrets.token_urlsafe(24)).
    std::shared_ptr<McpEndpoint> open_endpoint(const Json& tools, ToolHandler handler);
    // Revokes an endpoint; a call already running finishes first. Null does nothing.
    void close_endpoint(const std::shared_ptr<McpEndpoint>& endpoint);
    // http://127.0.0.1:<port>/mcp/<token>
    std::string url(const McpEndpoint& endpoint) const;
    // Endpoints open now.
    std::size_t endpoint_count() const;

    // The JSON-RPC answer to one message; nullopt for a notification. Blocks while a tool call
    // runs.
    std::optional<Json> handle_message(McpEndpoint& endpoint, const Json& message);
    // The HTTP answer to one request, as the socket gets it. Blocks while a tool call runs, so
    // the server calls it off the reactor.
    McpReply answer(const net::HttpRequest& request);

private:
    std::shared_ptr<McpEndpoint> endpoint_for(const std::string& token) const;
    Json call_tool(McpEndpoint& endpoint, const Json& params);
    std::optional<std::shared_ptr<McpEndpoint>> authorized(const net::HttpRequest& request,
                                                           McpReply& refused) const;

    struct Running;
    net::Reactor& reactor_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<McpEndpoint>> endpoints_;
    std::unique_ptr<Running> running_;
    std::uint16_t port_ = 0;
};

} // namespace fv::agents
