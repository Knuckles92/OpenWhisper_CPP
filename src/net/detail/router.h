#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "net/http_server.h"

// Starlette's Router over HttpServer's routes. No Boost here.
namespace fv::net::detail {

using Params = std::vector<std::pair<std::string, std::string>>;

// A compiled path pattern: literal text and "{name}" ([^/]+), "{name:str}", "{name:int}"
// ([0-9]+) or "{name:path}" (.*) parameters, matched against the whole decoded path with
// regex semantics (each parameter as long as the rest still matches).
class RoutePattern {
public:
    // Throws std::invalid_argument for a malformed pattern, an unknown converter or a parameter
    // named twice, as Starlette's compile_path refuses them.
    explicit RoutePattern(std::string_view pattern);
    std::optional<Params> match(std::string_view path) const;

private:
    struct Token {
        std::string text; // literal text, or the parameter's name
        enum class Kind { Literal, Segment, Digits, Path } kind;
    };
    bool match_from(std::size_t token, std::string_view path, std::size_t position,
                    Params& params) const;

    std::vector<Token> tokens_;
};

struct Route {
    std::string method;
    RoutePattern pattern;
    RouteOptions options;
    HttpServer::Handler handler;
};

struct RouteMatch {
    const Route* route = nullptr; // a full match: path and method
    Params params;
    // No full match: whether some route matched the path with another method (405), and the
    // methods of the first such route.
    bool path_known = false;
    std::vector<std::string> allowed;
    // No match at all, but the path with its trailing slash toggled matches (307 to it).
    std::optional<std::string> redirect_path;
};

// Affinity: not thread-safe; ServerImpl guards it.
class Router {
public:
    void add(Route route);
    // Routes in the order added; HEAD also matches a GET route.
    RouteMatch match(std::string_view method, std::string_view path) const;

private:
    std::vector<Route> routes_;
};

} // namespace fv::net::detail
