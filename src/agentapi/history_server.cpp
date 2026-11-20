#include "agentapi/history_server.h"

#include <chrono>
#include <format>
#include <utility>

#include "agentapi/agent_controls.h"
#include "agentapi/history_routes.h"
#include "agentapi/mcp_server.h"
#include "agentapi/mcp_tools.h"
#include "base/executor.h"
#include "net/address.h"
#include "net/tailscale.h"
#include "pyport/json.h"

namespace fv::agentapi {

namespace {

constexpr std::chrono::seconds kSsePing{15}; // sse-starlette's DEFAULT_PING_INTERVAL

std::vector<std::string> segments(std::string_view path) {
    std::vector<std::string> out;
    std::size_t start = 1;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        out.emplace_back(
            path.substr(start, slash == std::string_view::npos ? slash : slash - start));
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return out;
}

// protect's headers on every answer that passed through it: Cache-Control replaced, then
// X-Content-Type-Options.
void secure(net::Headers& headers) {
    const auto set = [&](std::string_view name, std::string_view value) {
        for (auto& [key, existing] : headers) {
            if (key == name) {
                existing = std::string(value);
                return;
            }
        }
        headers.emplace_back(std::string(name), std::string(value));
    };
    set("cache-control", "no-store");
    set("x-content-type-options", "nosniff");
}

std::string quote(std::string_view text, std::string_view safe) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        const bool keep = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                          (byte >= '0' && byte <= '9') || c == '_' || c == '.' || c == '-' ||
                          c == '~' || (byte < 0x80 && safe.find(c) != std::string_view::npos);
        if (keep) {
            out += c;
        } else {
            out += '%';
            out += hex[byte >> 4];
            out += hex[byte & 0x0f];
        }
    }
    return out;
}

std::string urlencode(const net::Query& query) {
    std::string out;
    for (const auto& [name, value] : query) {
        out += out.empty() ? "" : "&";
        const auto plus = [](std::string_view text) {
            std::string quoted = quote(text, " ");
            for (char& c : quoted) {
                c = c == ' ' ? '+' : c;
            }
            return quoted;
        };
        out += plus(name) + "=" + plus(value);
    }
    return out;
}

// Wraps the live API's responder so its answers carry protect's headers.
class SecuredResponder final : public net::Responder {
public:
    explicit SecuredResponder(std::shared_ptr<net::Responder> inner) : inner_(std::move(inner)) {}

    void send(int status, net::Headers headers, std::string body) override {
        secure(headers);
        inner_->send(status, std::move(headers), std::move(body));
    }
    void send_file(std::filesystem::path path, std::string content_type,
                   std::optional<net::ByteRange> range) override {
        inner_->send_file(std::move(path), std::move(content_type), range);
    }
    void send_file(std::filesystem::path path, std::string content_type,
                   std::optional<net::ByteRange> range, net::FileOptions options) override {
        secure(options.headers);
        inner_->send_file(std::move(path), std::move(content_type), range, std::move(options));
    }
    std::shared_ptr<net::EventStream> start_sse(net::Headers headers) override {
        secure(headers);
        return inner_->start_sse(std::move(headers));
    }

private:
    std::shared_ptr<net::Responder> inner_;
};

void ping_later(Executor& executor, std::shared_ptr<net::EventStream> stream, const Clock& clock) {
    TimerHandle timer =
        executor.post_after(kSsePing, [&executor, stream = std::move(stream), &clock]() mutable {
            if (stream->write(sse_ping_frame(clock))) {
                ping_later(executor, std::move(stream), clock);
            }
        });
    timer.detach();
}

} // namespace

void LiveRoutes::route(std::string method, std::string pattern, Handler handler) {
    std::lock_guard lock(mutex_);
    routes_.push_back({std::move(method), segments(pattern), std::move(handler)});
}

std::optional<LiveRoutes::Match> LiveRoutes::match(std::string_view method,
                                                   std::string_view path) const {
    if (!path.starts_with('/')) {
        return std::nullopt;
    }
    const std::vector<std::string> parts = segments(path);
    std::lock_guard lock(mutex_);
    std::optional<Match> partial;
    for (const Route& route : routes_) {
        if (route.segments.size() != parts.size()) {
            continue;
        }
        Match found;
        bool same = true;
        for (std::size_t i = 0; i < parts.size() && same; ++i) {
            const std::string& segment = route.segments[i];
            if (segment.size() > 2 && segment.front() == '{' && segment.back() == '}') {
                same = !parts[i].empty();
                found.params.emplace_back(segment.substr(1, segment.size() - 2), parts[i]);
            } else {
                same = segment == parts[i];
            }
        }
        if (!same) {
            continue;
        }
        if (route.method == method) {
            found.handler = route.handler;
            return found;
        }
        if (!partial) {
            // Starlette answers 405 with the first route that matched the path alone.
            found.allowed.push_back(route.method);
            partial = std::move(found);
        }
    }
    return partial;
}

void LiveRoutes::set_enabled(std::function<bool()> enabled) {
    std::lock_guard lock(mutex_);
    enabled_ = std::move(enabled);
}

std::function<bool()> LiveRoutes::enabled() const {
    std::lock_guard lock(mutex_);
    return enabled_;
}

void LiveRoutes::add_openapi_paths(const Json& paths) {
    std::lock_guard lock(mutex_);
    for (const auto& [path, operations] : paths.items()) {
        openapi_paths_[path] = operations;
    }
}

Json LiveRoutes::openapi_paths() const {
    std::lock_guard lock(mutex_);
    return openapi_paths_;
}

McpApp::McpApp(McpAppOptions options) : options_(std::move(options)) {
    live_.set_enabled(options_.enabled);
    history_ =
        std::make_unique<HistoryApi>(options_.database, options_.token, options_.allowed_hosts,
                                     options_.clock != nullptr ? *options_.clock : system_clock(),
                                     options_.history_clients, options_.local_history_only);
    AgentControlsOptions controls;
    controls.database = options_.database;
    controls.settings = options_.local_history_only ? nullptr : options_.settings;
    controls.allow_live_tools = options_.allow_live_tools && !options_.local_history_only;
    controls.enabled = options_.enabled;
    controls.on_change = options_.on_change;
    controls.meeting_renamer = options_.meeting_renamer;
    controls_ = std::make_unique<AgentControls>(std::move(controls));
    McpToolsOptions tools;
    tools.history = history_.get();
    tools.token = options_.token;
    tools.controls = controls_.get();
    tools.enabled = options_.enabled;
    tools.settings = options_.local_history_only ? nullptr : options_.settings;
    tools.local_history_only = options_.local_history_only;
    tools.allow_live_tools = options_.allow_live_tools;
    tools_ = std::make_unique<McpTools>(std::move(tools));
    server_ = std::make_unique<McpServer>(*tools_);
}

McpApp::~McpApp() = default;

McpServer& McpApp::protocol() {
    return *server_;
}

LiveRoutes& McpApp::live_routes() {
    return live_;
}

HistoryApi& McpApp::history() {
    return *history_;
}

AgentControls& McpApp::controls() {
    return *controls_;
}

std::optional<ApiResponse> McpApp::outer_checks(const net::HttpRequest& request) const {
    // Disabling cuts off new requests at once, even while the listener drains work already in
    // flight.
    if (options_.enabled && !options_.enabled()) {
        return error_response(503, "disabled", "MCP access is turned off.",
                              {{"Cache-Control", "no-store"}});
    }
    if (!options_.allowed_hosts.empty()) {
        const auto address = net::parse_address(request.peer.address);
        const bool local = address && net::is_loopback(*address);
        if (!local && !net::is_tailscale_address(request.peer.address)) {
            return error_response(403, "forbidden",
                                  "Only local or Tailscale clients are supported.",
                                  {{"Cache-Control", "no-store"}});
        }
    }
    return std::nullopt;
}

McpHttpAnswer McpApp::answer(const net::HttpRequest& request) {
    if (auto refused = outer_checks(request)) {
        return {std::move(*refused)};
    }
    if (auto refused = history_->refusal(request)) {
        secure(refused->headers);
        return {std::move(*refused)};
    }
    if (request.path == MCP_PATH) {
        McpHttpAnswer out = serve_mcp(*server_, request, options_.allowed_hosts);
        secure(out.response.headers);
        return out;
    }
    if (!live_.match(request.method, request.path) && request.path != "/") {
        // Starlette's redirect_slashes toward /mcp or a live route; the History API redirects
        // to its own routes itself.
        std::string other = request.path;
        if (other.ends_with('/')) {
            while (other.ends_with('/')) {
                other.pop_back();
            }
        } else {
            other += '/';
        }
        if (other == MCP_PATH || live_.match(request.method, other)) {
            std::string location = std::string(request.tls ? "https" : "http") + "://" +
                                   std::string(net::header(request.headers, "host").value_or("")) +
                                   other;
            const std::string query = urlencode(request.query);
            if (!query.empty()) {
                location += "?" + query;
            }
            ApiResponse redirect;
            redirect.status = 307;
            redirect.headers.emplace_back("location", quote(location, ":/%#?=@[]!$&'()*+,;"));
            secure(redirect.headers);
            return {std::move(redirect)};
        }
    }
    if (auto live = live_.match(request.method, request.path); live && !live->handler) {
        std::string allow;
        for (const std::string& method : live->allowed) {
            allow += (allow.empty() ? "" : ", ") + method;
        }
        ApiResponse refused =
            error_response(405, "method_not_allowed", "Method Not Allowed", {{"Allow", allow}});
        secure(refused.headers);
        return {std::move(refused)};
    }
    ApiResponse response = history_->handle(request);
    if (request.path == "/openapi.json" && response.status == 200 && !response.body.empty()) {
        // The live routes are part of this listener's contract, after the History API's.
        const Json live = live_.openapi_paths();
        if (!live.empty()) {
            Json schema = py::loads(response.body);
            for (const auto& [path, operations] : live.items()) {
                schema["paths"][path] = operations;
            }
            response.body = render_json(schema);
        }
    }
    return {std::move(response)};
}

void McpApp::serve(const net::HttpRequest& request, std::shared_ptr<net::Responder> responder) {
    if (!outer_checks(request) && !history_->refusal(request) && request.path != MCP_PATH) {
        if (auto live = live_.match(request.method, request.path); live && live->handler) {
            net::HttpRequest routed = request;
            routed.params = std::move(live->params);
            live->handler(routed, std::make_shared<SecuredResponder>(std::move(responder)));
            return;
        }
    }
    McpHttpAnswer out = answer(request);
    if (out.stream) {
        std::shared_ptr<net::EventStream> stream =
            responder->start_sse(std::move(out.response.headers));
        if (options_.pinger != nullptr && stream) {
            ping_later(*options_.pinger, std::move(stream),
                       options_.clock != nullptr ? *options_.clock : system_clock());
        }
        return;
    }
    responder->send(out.response.status, std::move(out.response.headers),
                    std::move(out.response.body));
}

std::string sse_ping_frame(const Clock& clock) {
    using namespace std::chrono;
    const auto now = time_point_cast<microseconds>(clock.utc());
    const auto day = floor<days>(now);
    const year_month_day date{day};
    const hh_mm_ss<microseconds> time{now - day};
    std::string text =
        std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", static_cast<int>(date.year()),
                    static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                    time.hours().count(), time.minutes().count(), time.seconds().count());
    if (time.subseconds().count() != 0) {
        text += std::format(".{:06}", time.subseconds().count());
    }
    return ": ping - " + text + "+00:00\r\n\r\n";
}

} // namespace fv::agentapi
