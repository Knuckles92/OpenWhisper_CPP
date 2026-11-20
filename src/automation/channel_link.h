#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "base/cancel.h"
#include "base/env.h"
#include "base/json.h"
#include "http/client.h"

// How the Claude Code channel (openwhisper-cpp-cli --channel) reaches the running app
// (services/automation/channel_bridge.py: AppLink and EventStream): it follows the live API's
// event stream (GET /v1/events?types=channel.message on the in-app MCP listener) and shows
// Claude's replies with POST /v1/notify. Logs never carry dictated text, replies or the token.
namespace fv::automation {

struct SseEvent;

// Reconnect backoff, timeouts and the limits of what the channel sends.
inline constexpr std::chrono::milliseconds CHANNEL_BACKOFF_FIRST{1000};
inline constexpr std::chrono::milliseconds CHANNEL_BACKOFF_MAX{30000};
inline constexpr std::chrono::milliseconds CHANNEL_CONNECT_TIMEOUT{5000};
// An idle stream gets a ping comment every 15 s; three missed ones mean it is dead.
inline constexpr std::chrono::milliseconds CHANNEL_READ_TIMEOUT{45000};
inline constexpr std::chrono::milliseconds CHANNEL_NOTIFY_TIMEOUT{5000};
// Code points.
inline constexpr std::size_t MAX_REPLY_CHARS = 500;
inline constexpr std::size_t MAX_TITLE_CHARS = 80;
inline constexpr std::size_t MAX_META_CHARS = 200;

// The live API counts streams that send the client header with this value as listening channel
// sessions (agentapi::CHANNEL_CLIENT).
inline constexpr std::string_view CHANNEL_CLIENT_NAME = "claude-channel";
inline constexpr std::string_view EVENTS_PATH = "/v1/events?types=channel.message";
inline constexpr std::string_view NOTIFY_PATH = "/v1/notify";

// "X-<product>-Client".
std::string channel_client_header();
// The overrides of the app's address and token: "<env_prefix>URL" and "<env_prefix>TOKEN", and
// the legacy product's names, read when the new ones are unset or empty.
std::string url_env();
std::string token_env();
std::string legacy_url_env();
std::string legacy_token_env();

// The app can't be asked right now; state() names why for the logs ("bad_url", "no_token",
// "unreachable", "timeout", "http_<status>"), what() is the sentence for Claude or the log.
class LinkError : public std::runtime_error {
public:
    LinkError(std::string state, const std::string& message)
        : std::runtime_error(message), state_(std::move(state)) {}
    const std::string& state() const noexcept { return state_; }
    std::string message() const { return what(); }

private:
    std::string state_;
};

// Affinity: value type.
struct Target {
    std::string host;
    int port = 0;
    bool https = false;
    std::string base_path;

    // "host:port".
    std::string label() const;
    // "localhost" in any case, or a loopback address (an IPv4-mapped one unwrapped).
    bool on_this_computer() const;
    friend bool operator==(const Target&, const Target&) = default;
};

// The in-app listener's port as McpRuntime.restore reads it: mcp_port, 1 to 65535, else 8767.
int configured_port(const Json& settings);

// <env_prefix>URL as a target. Throws LinkError("bad_url", "<env_prefix>URL must be an http://
// address such as http://127.0.0.1:8767.") for anything but an http:// or https:// address with
// a host and a valid port, and py::ValueError where urllib's urlsplit itself raises (an unmatched
// bracket, a bracketed host that is not an IPv6 address).
Target parse_url(std::string_view url);

// _error_message: the "error.message" text of a JSON answer; "" when there is none.
std::string error_message(std::string_view body);
// What Claude reads when POST /v1/notify answered `status`.
std::string reply_refusal(int status, std::string_view body);
// (state, message) of an event stream request the app refused.
std::pair<std::string, std::string> stream_refusal(int status, std::string_view body);

// The settings file of this data root as a mapping, read afresh; throws when it cannot be read
// or is not JSON (Python's load_all_settings gives {} then, which the callers' fallbacks match).
Json saved_settings();
// The MCP access token from the OS credential store, through a store made for this one read:
// the app creates the token on first use while the channel runs, and a store caches a miss.
std::optional<std::string> saved_token();

// Affinity: value type.
struct AppLinkOptions {
    std::string url;   // the URL override; "" uses the saved port on 127.0.0.1
    std::string token; // the token override; "" reads the saved one
    std::function<Json()> read_settings = saved_settings;
    std::function<std::optional<std::string>()> read_token = saved_token;
    std::string version; // for the User-Agent; "" sends "0"
    // Null: a curl client of its own.
    std::shared_ptr<http::Client> client;
};

// Where the running app listens, and the token it expects. The URL and token overrides win over
// the app's own settings and credential store; otherwise the port is read from the settings file
// on every connection (the user may change it) and the token from the credential store once, and
// again after the app refuses it. The saved token is only ever sent to this computer: another
// computer needs https and a token of its own.
// Affinity: thread-safe.
class AppLink {
public:
    explicit AppLink(AppLinkOptions options = {});
    virtual ~AppLink();
    AppLink(const AppLink&) = delete;
    AppLink& operator=(const AppLink&) = delete;

    // The overrides from <env_prefix>URL and <env_prefix>TOKEN (the legacy names when unset or
    // empty).
    static AppLinkOptions from_environment(const EnvReader& env, std::string version);

    // Throws LinkError (bad_url, no_token) or py::ValueError as parse_url.
    Target target() const;
    // "<product> isn't running, or API & MCP is off in its Settings.", or "API & MCP is off in
    // <product> Settings." when no URL is set and the saved mcp_enabled is false.
    std::string unreachable_message() const;
    // The token to send; throws LinkError("no_token") while none can be read.
    std::string token();
    // The app refused the token: read it again before the next request.
    void forget_token();
    // Authorization, the client header, User-Agent, then `extra`, in that order.
    http::Headers headers(const std::string& token, http::Headers extra = {}) const;
    // The URL of `path` (with any query) at `target`.
    static std::string url(const Target& target, std::string_view path);
    http::Client& client() const { return *client_; }

    // Shows `text` near the pointer (POST /v1/notify). Throws LinkError with copy for Claude.
    // A rejected saved token is read again and the request sent once more.
    virtual void notify(const std::string& text, const std::string& title = {});

    // Whether something accepts connections at the target (Python connected before it read the
    // token, so a missing token reads as "unreachable" while the app is away).
    virtual bool reachable(const Target& target) const;

    // Ends a request in progress and fails every later one at once (LinkError "unreachable"):
    // for shutdown, where Python's daemon reply threads simply died with the process instead of
    // holding it up for their timeouts.
    void cancel_requests();

private:
    std::pair<int, std::string> post(std::string_view path, const std::string& payload);

    const std::string url_;
    const std::string fixed_token_;
    const std::function<Json()> read_settings_;
    const std::function<std::optional<std::string>()> read_token_;
    const std::string user_agent_;
    std::shared_ptr<http::Client> client_;
    std::mutex mutex_;
    std::optional<std::string> token_;
    CancelSource requests_cancel_;
};

// What the channel server starts once Claude Code has initialized, and stops at the end.
// Affinity: start and stop from one thread at a time.
class ChannelStream {
public:
    virtual ~ChannelStream() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
};

// Affinity: value type.
struct EventStreamOptions {
    // Called (on the stream's thread, which then ends) when the app is unreachable while an
    // update replaces it; null: never.
    std::function<void()> on_update;
    // Whether an update or setup is replacing the installed app now.
    std::function<bool()> update_check;
    std::chrono::milliseconds backoff_first = CHANNEL_BACKOFF_FIRST;
    std::chrono::milliseconds backoff_max = CHANNEL_BACKOFF_MAX;
    std::chrono::milliseconds read_timeout = CHANNEL_READ_TIMEOUT;
};

// Follows the app's channel messages on a thread ("channel-events") and hands each to `deliver`
// as notifications/claude/channel params. Reconnects with backoff (1 s doubling to 30 s, back to
// 1 s after a stream opened) and resumes with Last-Event-ID. Logs one line per change of state,
// never per retry. An events.gap whose reason is "evicted" is told to Claude once per resume
// point.
// Affinity: thread-safe; `link` outlives the stream, `deliver` may be called on its thread.
class EventStream final : public ChannelStream {
public:
    EventStream(AppLink& link, std::function<void(const Json& params)> deliver,
                EventStreamOptions options = {});
    // stop().
    ~EventStream() override;
    EventStream(const EventStream&) = delete;
    EventStream& operator=(const EventStream&) = delete;

    void start() override;
    // Ends the request in progress and waits for the thread (unless called on it).
    void stop() override;

    // The last state reported: "", "connected", "dropped", "silent", "unreachable", "forbidden",
    // "unauthorized", "unsupported", "bad_url", "no_token", "http_<status>".
    std::string state() const;
    // The resume point: the id of the last event block the stream ended.
    std::string last_id() const;
    // Whether the thread has started and not yet ended.
    bool running() const;

    // _handle: one parsed event.
    void handle(const SseEvent& event);

private:
    void run();
    // One connection: whether a stream opened, and the state it ended in.
    std::pair<bool, std::string> follow_once();
    std::pair<bool, std::string> connection_lost(bool opened);
    void report(const std::string& state, const std::string& message, bool warning = false);
    void announce_gap(const SseEvent& event);
    bool stopping() const { return stop_.load(); }

    AppLink& link_;
    const std::function<void(const Json&)> deliver_;
    const EventStreamOptions options_;

    mutable std::mutex mutex_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
    CancelSource cancel_;
    std::string state_;
    std::string last_id_;
    // The resume point whose gap Claude was last told about.
    std::optional<std::string> gap_announced_at_;
};

// channel.message's origin as the channel passes it on: "dictated" when the app says these are
// the user's own words, else "program".
std::string origin_of(const Json& data);
// The entries Claude Code keeps: identifier keys with non-empty one-line string values, at most
// MAX_META_CHARS code points each, in order.
Json clean_meta(const Json& meta);
// {"content": text, "meta": {origin, job_id, action, app}} for one channel.message event; nullopt
// when it carries no text.
std::optional<Json> channel_params(const Json& event);
// _gap_reason: an events.gap event's data.reason; "" when it has none.
std::string gap_reason(const SseEvent& event);
// The origin of the channel's own notices: the product in lower case.
std::string channel_notice_origin();
// What Claude reads when the event stream says messages were missed.
std::string gap_notice();

} // namespace fv::automation
