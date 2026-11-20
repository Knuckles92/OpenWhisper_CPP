#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agentapi/api_errors.h"
#include "agentapi/sse_stream.h"
#include "automation/event_bus.h"
#include "base/clock.h"
#include "base/json.h"
#include "net/http_server.h"

// The live API on the in-app listener (services/automation/api.py): the app's state, its event
// stream (GET /v1/events, server-sent events), its actions, recording, filter leases and notices.
// Only the MCP listener serves these routes, behind its MCP-off check, its peer check and the
// History API's token gate; the headless openwhisper-cpp-cli --api never does, because no running
// app is behind it. Every request rereads the user's permissions (Settings > API & MCP). Calls into
// the app go through the registered automation::AppControl, which may block for about four
// seconds, so the routes run on the listener's io executor, never on the reactor.
namespace fv {
class Executor;
class SettingsStore;
namespace automation {
class FilterLeases;
}
} // namespace fv

namespace fv::agentapi {

class LiveRoutes;
class McpRuntime;

// EventBus::subscribers() labels of the two kinds of stream.
inline constexpr std::string_view SSE_LABEL = "sse";
inline constexpr std::string_view CHANNEL_LABEL = "claude-channel";
// A stream whose request carries the client header with this value is a Claude Code channel.
inline constexpr std::string_view CHANNEL_CLIENT = "claude-channel";
// The client header, lower case: "x-<product>-client", and the one OpenWhisper's channel sends,
// which is accepted too.
std::string client_header();
std::string legacy_client_header();

// Lengths in code points; bodies in bytes, with room for the longest text even when every
// character arrives as a JSON escape.
inline constexpr std::size_t MAX_ACTION_TEXT = 20'000;
inline constexpr std::size_t MAX_NOTICE_TEXT = 500;
inline constexpr std::size_t MAX_NOTICE_TITLE = 100;
inline constexpr std::size_t MAX_ID_CHARS = 64;
inline constexpr std::size_t MAX_URL_CHARS = 2048;
inline constexpr std::size_t MAX_RUN_BODY = 256 * 1024;
inline constexpr std::size_t MAX_SMALL_BODY = 16 * 1024;
// FastAPI's limits on /v1/events' query and headers, in code points (headers in bytes, which
// Starlette reads as Latin-1).
inline constexpr std::size_t MAX_TYPES_CHARS = 2000;
inline constexpr std::size_t MAX_HEADER_CHARS = 64;

// _Invalid: a request the API cannot accept; what() is shown to the caller.
class Invalid : public std::runtime_error {
public:
    explicit Invalid(const std::string& message, int status = 422,
                     std::string code = "invalid_parameters");
    int status() const noexcept { return status_; }
    const std::string& code() const noexcept { return code_; }
    std::string message() const { return what(); }

private:
    int status_;
    std::string code_;
};

// ?types=a,b as the event types it names (sorted, without repeats); nullopt for every type
// (absent, or only commas and whitespace). Throws Invalid("types names an event type <product>
// doesn't publish.") for a name that is not in the catalog.
std::optional<std::vector<std::string>> parse_types(std::optional<std::string_view> raw);
// The Last-Event-ID header as a sequence number; nullopt when absent or blank. Throws
// Invalid("Last-Event-ID must be the id of an event from this stream.") unless it is 1 to 18
// digits once stripped. As str.isdigit() counts the superscripts ¹ ² ³ that a Latin-1 header can
// carry, and int() then refuses them, those throw py::ValueError (an unexpected failure).
std::optional<std::int64_t> parse_last_event_id(std::optional<std::string_view> raw);
// One server-sent events message: "id: <seq>" (with_id), "event: <type>", then "data: " and the
// event's JSON in ASCII (every line break, U+2028 included, is an escape), ending in a blank line.
std::string sse_frame(const automation::Event& event, bool with_id = true);
// The synthetic events.gap event (seq 0); it never goes through the bus. Its time is the clock's.
automation::Event gap_event(std::int64_t missed_from_seq, std::string_view reason,
                            const Clock& clock = system_clock());
// The request's JSON object, with a size limit; {} for a body of ASCII whitespace. Throws
// Invalid: 413 "request_too_large" for a declared Content-Length or a body over `limit`, 422
// "The request body must be a JSON object." for anything else that is not one. The body is
// decoded as json.loads decodes bytes: UTF-8 (a BOM allowed), UTF-16 or UTF-32, lone surrogates
// kept.
Json read_json_body(const net::HttpRequest& request, std::size_t limit);
// Whether a request came from this computer: a loopback address, IPv4-mapped addresses
// unwrapped and an IPv6 scope id ignored. Anything else, a name included, is another computer.
bool peer_is_local(std::string_view host);
// False for text holding a lone UTF-16 surrogate (half an emoji), which cannot become UTF-8.
bool is_unicode_text(std::string_view utf8);
// _string: body[key] as text. Missing or null is "" unless required; otherwise Invalid with
// "<label> must be text of 1 to <max> characters." (required) or "... of at most <max>
// characters.", and "<label> must be valid Unicode text." for a lone surrogate.
std::string string_field(const Json& body, std::string_view key, bool required,
                         std::size_t max_chars, std::string_view label = {});

// _LiveRoutes and create_live_router: the eleven live routes. The routes' handlers answer on the
// thread that calls them (the listener's io executor); event streams time their pings and checks
// on `io`. Everything passed in must outlive this object, which ends its open streams when it is
// destroyed.
// Affinity: thread-safe.
class LiveApi {
public:
    // `enabled`: false while the live API must not stream (null: always true); a listener's own
    // enabled() (MCP switched off, the listener stopping) is honoured as well. `bus` is the one
    // the streams follow and the state counts listeners on; `leases` the filters leased here.
    LiveApi(SettingsStore& settings, std::function<bool()> enabled, automation::EventBus& bus,
            automation::FilterLeases& leases, Executor& io, const Clock& clock = system_clock());
    ~LiveApi();
    LiveApi(const LiveApi&) = delete;
    LiveApi& operator=(const LiveApi&) = delete;

    // Adds the routes, in OpenWhisper's order, and their paths to the listener's /openapi.json.
    void install(LiveRoutes& routes);
    // The intervals and limits of streams opened from now on (tests shorten them).
    void set_stream_options(StreamOptions options);
    // Ends every open event stream now, without a frame.
    void close_streams();
    // Forgets every filter lease (a listener that served has stopped: they cannot be renewed).
    void drop_leases();

private:
    struct Context;
    std::shared_ptr<Context> context_;
};

// Mounts `api` on every listener `runtime` starts and drops its leases each time one that served
// stops (app.py's lifespan). Both must outlive the runtime's listeners.
void mount_live_api(McpRuntime& runtime, LiveApi& api);

// The paths the live routes add to /openapi.json (FastAPI's document of create_live_router, in
// route order), with the product named in their descriptions.
const Json& live_openapi_paths();

} // namespace fv::agentapi
