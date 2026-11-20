#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentapi/api_errors.h"
#include "base/clock.h"
#include "net/http_server.h"

// The authenticated, loopback-only v1 History API (services/agent_api/app.py) as one pure
// function from request to answer: the gate, the routes of services/agent_api (GET only), FastAPI's
// parameter validation (422) and the error envelope. Nothing here opens a socket: the in-app MCP
// server mounts it (M5.9) and openwhisper-cpp-cli --api serves it (M5.20).
namespace fv::remote {
class ClientHistorySource;
}

namespace fv::agentapi {

class HistoryStore;
class FederatedHistory;
namespace detail {
class StandaloneHistory;
} // namespace detail

// _check_token: 32 to 512 printable ASCII characters without whitespace. Throws
// std::invalid_argument("OPENWHISPER_CPP_API_TOKEN must be 32–512 printable ASCII characters
// without whitespace.") otherwise.
void check_token(std::string_view token);

// Affinity: thread-safe; handle() blocks on SQLite, so it runs off the reactor and the GUI
// thread (Python ran these handlers in a thread pool).
class HistoryApi {
public:
    // create_app: checks the token, then opens the store (both throw std::invalid_argument; the
    // store also sql::Error). `allowed_hosts` are Host names accepted beside 127.0.0.1 and
    // localhost (the MCP server's Tailscale address and name), lower case. `clock` times the
    // cursors kept for requests that name a paired computer.
    HistoryApi(std::filesystem::path database, std::string token,
               std::vector<std::string> allowed_hosts = {}, const Clock& clock = system_clock(),
               std::shared_ptr<remote::ClientHistorySource> clients = {}, bool local_only = false);
    ~HistoryApi();
    HistoryApi(const HistoryApi&) = delete;
    HistoryApi& operator=(const HistoryApi&) = delete;

    // The gate (A20-03) then the route; also what MCP history tools call in process (M5.9).
    // Every answer carries Cache-Control: no-store and X-Content-Type-Options: nosniff, except
    // the 500 of an unexpected failure, which Python's outermost middleware wrote without them.
    // Uses method, path (decoded), query and headers; never the body, peer or `params`.
    ApiResponse handle(const net::HttpRequest&);
    // _refusal: 403 "forbidden" for a Host outside the trusted names or any Origin header, then
    // 401 "unauthorized" with WWW-Authenticate: Bearer unless Authorization is "Bearer <token>"
    // (scheme in any case, compared in constant time). Without the security headers.
    std::optional<ApiResponse> refusal(const net::HttpRequest&) const;

    HistoryStore& store();

private:
    ApiResponse route(const net::HttpRequest&);

    std::string token_;
    bool local_only_;
    std::vector<std::string> trusted_hosts_;
    std::unique_ptr<HistoryStore> store_;
    std::unique_ptr<detail::StandaloneHistory> history_;
    std::unique_ptr<FederatedHistory> federation_;
};

// The OpenAPI document /openapi.json serves (tools/gen/agent_openapi.py), titled
// identity().product + " History API", as compact JSON text.
const std::string& openapi_document();

} // namespace fv::agentapi
