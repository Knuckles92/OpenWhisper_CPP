#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "agentapi/api_errors.h"
#include "net/http_server.h"

// The MCP SDK's stateless Streamable HTTP transport in JSON-response mode, as the in-app server
// mounts it at /mcp (services/agent_mcp/app.py _mcp_http_app): a body cap of 64 KiB, the
// DNS-rebinding Host allowlist, the Accept and Content-Type checks, and the routing of each POST
// to the handshake wire or, by its MCP-Protocol-Version header, to the per-request wire.
namespace fv::agentapi {

class McpServer;

inline constexpr std::size_t MCP_MAX_BODY = 64 * 1024; // bytes

// What the transport answers. `stream` asks the listener to hold the answer open as an empty
// server-sent event stream with `response.headers` (a GET of the handshake wire that accepts
// text/event-stream: the SDK's standalone notification stream, which a stateless server never
// writes to), pinged every 15 s until the client leaves.
struct McpHttpAnswer {
    ApiResponse response;
    bool stream = false;
};

// One request to /mcp. `allowed_hosts` are the Host names accepted beside 127.0.0.1 and
// localhost, each also with any port. The History API's gate has already passed; the caller adds
// its security headers.
// Affinity: thread-safe; blocks while a tool runs.
McpHttpAnswer serve_mcp(const McpServer& server, const net::HttpRequest& request,
                        const std::vector<std::string>& allowed_hosts);

} // namespace fv::agentapi
