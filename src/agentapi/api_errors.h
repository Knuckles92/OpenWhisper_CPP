#pragma once

#include <string>
#include <string_view>

#include "base/json.h"
#include "net/types.h"

// What the History API answers (services/agent_api/app.py), as values: the handlers are pure
// functions, so the in-app MCP server and openwhisper-cpp-cli --api serve them over net::HttpServer
// and the tests call them directly. The MCP server and the live API reuse error_response for their
// own errors, as OpenWhisper reuses app.py's _error.
namespace fv::agentapi {

// One HTTP answer. `headers` hold everything but Content-Length, which the server adds from the
// body, in the order Starlette sends them; names are lower case.
struct ApiResponse {
    int status = 200;
    net::Headers headers;
    std::string body;
};

// Starlette's JSONResponse: `extra` (names lowered), then content-type application/json; the body
// is json.dumps(value, ensure_ascii=False, separators=(",", ":")).
ApiResponse json_response(int status, const Json& value, net::Headers extra = {});

// _error: {"error":{"code":...,"message":...}} with `status`.
ApiResponse error_response(int status, std::string_view code, std::string_view message,
                           net::Headers extra = {}); // {"error":{"code","message"}}

// What Starlette's ServerErrorMiddleware answers for an exception no handler took: 500,
// "Internal Server Error" as text/plain. It bypasses the History API's middleware, so it carries
// no security headers.
ApiResponse internal_error_response();

// The body text a JSONResponse renders for `value`.
std::string render_json(const Json& value);

} // namespace fv::agentapi
