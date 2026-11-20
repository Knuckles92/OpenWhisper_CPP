#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agentapi/api_errors.h"
#include "agentapi/mcp_http.h"
#include "base/clock.h"
#include "base/json.h"
#include "net/http_server.h"

// What the in-app MCP listener serves (services/agent_mcp/app.py create_app): the History API
// at /v1 and /openapi.json, the MCP endpoint at /mcp and the live API's routes, behind one chain
// of checks, outermost first: MCP turned off (503 "disabled"), a peer that is neither this
// computer nor a Tailscale address when Tailscale access is on (403), then the History API's
// Host, Origin and bearer-token gate and its security headers. /mcp is the only MCP path, so
// every other unknown path or method keeps the API's JSON 404 and 405.
namespace fv {
class Executor;
class SettingsStore;
} // namespace fv

namespace fv::remote {
class ClientHistorySource;
}

namespace fv::agentapi {

class AgentControls;
class HistoryApi;
class McpServer;
class McpTools;

// The live API's routes (M5.16), served behind the listener's checks. A "{name}" segment matches
// one path segment and arrives in HttpRequest::params; a path whose routes do not take the
// method answers 405 with their methods in Allow.
// Affinity: thread-safe; add routes before the listener serves.
class LiveRoutes {
public:
    using Handler = std::function<void(const net::HttpRequest&, std::shared_ptr<net::Responder>)>;

    void route(std::string method, std::string pattern, Handler handler);

    struct Match {
        Handler handler;                  // set when a route takes the method
        std::vector<std::string> allowed; // the first matching route's methods otherwise
        std::vector<std::pair<std::string, std::string>> params;
    };
    // nullopt when no route's pattern matches the path.
    std::optional<Match> match(std::string_view method, std::string_view path) const;

    // The listener's enabled() (McpAppOptions::enabled), which a route that holds a request open
    // (an event stream) keeps asking: false once MCP is switched off or the listener stops.
    // Null when the listener has none.
    void set_enabled(std::function<bool()> enabled);
    std::function<bool()> enabled() const;
    // The routes' entries for the listener's /openapi.json, {path: {method: operation}}, added
    // after the History API's paths, as FastAPI lists an included router's routes.
    void add_openapi_paths(const Json& paths);
    Json openapi_paths() const;

private:
    struct Route {
        std::string method;
        std::vector<std::string> segments;
        Handler handler;
    };
    mutable std::mutex mutex_;
    std::vector<Route> routes_;
    std::function<bool()> enabled_;
    Json openapi_paths_ = Json::object();
};

struct McpAppOptions {
    std::filesystem::path database;
    std::string token;
    // The Tailscale address and MagicDNS name, when Tailscale access is on; with any, peers must
    // be loopback or Tailscale addresses.
    std::vector<std::string> allowed_hosts;
    std::function<bool()> enabled; // null: no MCP-off check
    SettingsStore* settings = nullptr;
    std::function<void(std::string kind, Json changes)> on_change;
    std::function<void(std::string id, std::string title)> meeting_renamer;
    const Clock* clock = nullptr; // the History API's cursor clock; null: the system clock
    // Where the pings of a held-open event stream are timed; null: no pings.
    Executor* pinger = nullptr;
    std::shared_ptr<remote::ClientHistorySource> history_clients;
    bool local_history_only = false;
    bool allow_live_tools = true;
};

// Affinity: thread-safe; blocking (SQLite, tools), so the listener runs it off the reactor.
class McpApp {
public:
    // Opens the History API (std::invalid_argument for a bad token or database, sql::Error).
    explicit McpApp(McpAppOptions options);
    ~McpApp();
    McpApp(const McpApp&) = delete;
    McpApp& operator=(const McpApp&) = delete;

    // Everything but the live routes, which it answers 404 or 405 like unknown paths: a pure
    // function from request to answer, for tests and the listener.
    McpHttpAnswer answer(const net::HttpRequest& request);
    // The listener's dispatch: answer(), a live route, or an empty event stream held open.
    void serve(const net::HttpRequest& request, std::shared_ptr<net::Responder> responder);

    McpServer& protocol();
    LiveRoutes& live_routes();
    HistoryApi& history();
    AgentControls& controls();

private:
    std::optional<ApiResponse> outer_checks(const net::HttpRequest& request) const;

    McpAppOptions options_;
    std::unique_ptr<HistoryApi> history_;
    std::unique_ptr<AgentControls> controls_;
    std::unique_ptr<McpTools> tools_;
    std::unique_ptr<McpServer> server_;
    LiveRoutes live_;
};

// Python's str(datetime.now(timezone.utc)) as sse-starlette pings with it: ": ping - <time>".
std::string sse_ping_frame(const Clock& clock);

} // namespace fv::agentapi
