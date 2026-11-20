#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

// The MCP protocol of the in-app server, as the Python MCP SDK 2.2 (MCPServer over its low-level
// server) answers it: JSON-RPC 2.0 on two wires. The handshake wire (protocol revisions
// 2024-11-05 to 2025-11-25) has initialize and ping; the per-request wire (2026-07-28) carries the
// protocol version and client capabilities in every request's params._meta, has server/discover
// instead, and wraps results in resultType and a serverInfo _meta stamp. Both have tools/list,
// tools/call and the empty resource and prompt lists. Shapes and key orders are the SDK's
// (tests/fixtures/transcripts/agent_mcp.json pins them).
namespace fv::net {
struct HttpRequest;
}

namespace fv::agentapi {

class McpTools;

// JSON-RPC error codes.
inline constexpr int PARSE_ERROR = -32700;
inline constexpr int INVALID_REQUEST = -32600;
inline constexpr int METHOD_NOT_FOUND = -32601;
inline constexpr int INVALID_PARAMS = -32602;
inline constexpr int INTERNAL_ERROR = -32603;
inline constexpr int HEADER_MISMATCH = -32020;
inline constexpr int MISSING_REQUIRED_CLIENT_CAPABILITY = -32021;
inline constexpr int UNSUPPORTED_PROTOCOL_VERSION = -32022;

inline constexpr std::string_view LATEST_HANDSHAKE_VERSION = "2025-11-25";
inline constexpr std::string_view MODERN_VERSION = "2026-07-28";
// Revisions reachable through initialize, oldest first.
inline constexpr std::string_view HANDSHAKE_VERSIONS[] = {"2024-11-05", "2025-03-26", "2025-06-18",
                                                          "2025-11-25"};
inline constexpr std::string_view PROTOCOL_VERSION_META_KEY =
    "io.modelcontextprotocol/protocolVersion";
inline constexpr std::string_view CLIENT_CAPABILITIES_META_KEY =
    "io.modelcontextprotocol/clientCapabilities";
inline constexpr std::string_view CLIENT_INFO_META_KEY = "io.modelcontextprotocol/clientInfo";
inline constexpr std::string_view SERVER_INFO_META_KEY = "io.modelcontextprotocol/serverInfo";

bool is_handshake_version(std::string_view version);

// A JSON-RPC error a method answers with; data is left out when unset.
class RpcError : public std::runtime_error {
public:
    RpcError(int code, const std::string& message, std::optional<Json> data = std::nullopt)
        : std::runtime_error(message), code_(code), data_(std::move(data)) {}
    int code() const noexcept { return code_; }
    const std::optional<Json>& data() const noexcept { return data_; }
    // {"code", "message", "data"?}
    Json to_json() const;

private:
    int code_;
    std::optional<Json> data_;
};

// One request the transport has accepted.
struct McpRequest {
    std::string method;
    Json params;         // an object, or null when absent
    Json id;             // a string or an integer
    bool modern = false; // the 2026-07-28 wire
    std::string protocol_version;
    const net::HttpRequest* http = nullptr; // the HTTP request it came in (peer, headers)
};

// Affinity: thread-safe; register methods and hooks before serving. Methods run on the caller's
// thread, which may block.
class McpServer {
public:
    // Answers `params` with a result object or throws RpcError. On the per-request wire the
    // server adds resultType and the serverInfo _meta stamp.
    using Handler = std::function<Json(const McpRequest&)>;

    explicit McpServer(McpTools& tools);

    // A method on both wires, beside the SDK's own; a later registration of a name replaces it
    // (events/list, events/subscribe and events/unsubscribe of MCP Events).
    void register_method(std::string method, Handler handler);
    // Edits the capabilities object of initialize and server/discover results on both wires (MCP
    // Events adds "events" while it is on).
    void add_capability_hook(std::function<void(Json& capabilities)> hook);

    // The result object of a request, or the error it is answered with.
    Json dispatch(const McpRequest& request) const; // throws RpcError

    // initialize's result (handshake wire).
    Json initialize_result(std::string_view requested_version) const;
    // server/discover's result before the per-request envelope is added.
    Json discover_result() const;

private:
    Json legacy(const McpRequest& request) const;
    Json modern(const McpRequest& request) const;
    Json capabilities(Json base) const;
    Json call_tool(const McpRequest& request) const;
    Json stamp(Json result) const;

    McpTools& tools_;
    mutable std::mutex mutex_;
    std::map<std::string, Handler, std::less<>> methods_;
    std::vector<std::function<void(Json&)>> hooks_;
};

} // namespace fv::agentapi
