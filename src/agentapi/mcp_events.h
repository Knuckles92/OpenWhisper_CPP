#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/json.h"
#include "net/address.h"

// MCP Events (services/agent_mcp/events_ext.py), the draft extension of the Triggers & Events
// working group in the shape ChatGPT documents: events/list, events/subscribe and
// events/unsubscribe on the in-app MCP server, and one signed webhook POST per event to each
// matching subscription. While MCP Events is on, initialize and server/discover advertise
// capabilities.events = {}. A subscription's id hashes the principal (everyone holding the MCP
// token), the callback URL, the event name and the canonical arguments, so subscribing again is a
// refresh. Callback URLs are verified with a signed challenge first. Subscriptions persist in a
// file without their secrets, which are in the credential store. Deliveries stop while MCP, MCP
// Events or live events are off, and when a subscription passes its refreshBefore. URLs, secrets
// and event data are never logged.
//
// A subscription remembers whether it came from this computer or from a Tailscale peer. A peer's
// subscriptions are delivered only while agents over Tailscale are allowed, a peer cannot name a
// callback on this computer or its local network (by address or by a name that resolves there),
// each request goes to the address that was checked, and a peer hears only that a verification
// or a delivery failed, never how.
namespace fv {
class CallbackGuard;
class CredentialStore;
class SettingsStore;
} // namespace fv

namespace fv::automation {
class EventBus;
class EventSubscription;
struct Event;
} // namespace fv::automation

namespace fv::http {
class Client;
}

namespace fv::agentapi {

class McpServer;
struct McpRequest;

namespace events_ext {

// The bus label, so bus().subscribers(BUS_LABEL) counts MCP Events.
inline constexpr std::string_view BUS_LABEL = "mcp-events";
inline constexpr std::size_t MAX_SUBSCRIPTIONS = 50;
// The draft's (and ChatGPT's) largest delivery body, in bytes.
inline constexpr std::size_t MAX_PAYLOAD_BYTES = 256 * 1024;
// Per request (connecting, and each wait for the answer), as ChatGPT's own sender.
inline constexpr double REQUEST_TIMEOUT_S = 10.0;
// The wait before the 2nd, 3rd and 4th try; there is no 5th.
inline constexpr std::array<double, 3> RETRY_DELAYS_S = {2.0, 10.0, 60.0};
// The longest wait a Retry-After header may ask for.
inline constexpr double MAX_RETRY_AFTER_S = 300.0;
inline constexpr std::int64_t DEFAULT_TTL_MS = 24LL * 60 * 60 * 1000;
inline constexpr std::int64_t MIN_TTL_MS = 60LL * 1000;
inline constexpr std::int64_t MAX_TTL_MS = 7LL * 24 * 60 * 60 * 1000;
// How long a verified callback URL skips a fresh challenge.
inline constexpr double VERIFY_CACHE_S = 60 * 60.0;
// How long deliveries carry the old secret's signature after a refresh changed it.
inline constexpr double ROTATION_WINDOW_S = 10 * 60.0;
// Bytes behind a client's whsec_ secret.
inline constexpr std::size_t SECRET_MIN_BYTES = 24;
inline constexpr std::size_t SECRET_MAX_BYTES = 64;
inline constexpr std::size_t MAX_QUEUED = 500;
inline constexpr std::size_t WORKERS = 4;
// Bytes of a callback's answer kept (a verification echo is tiny).
inline constexpr std::size_t MAX_REPLY_BYTES = 16 * 1024;
// Where a subscription was made: this computer, or a Tailscale peer.
inline constexpr std::string_view ORIGIN_LOCAL = "local";
inline constexpr std::string_view ORIGIN_TAILSCALE = "tailscale";

// JSON-RPC error codes. CALLBACK_ENDPOINT_ERROR is ChatGPT's documented -32015; the rest follow
// the draft's provisional numbering (SEP-3415). INVALID_PARAMS and INTERNAL_ERROR are
// mcp_server.h's.
inline constexpr int CALLBACK_ENDPOINT_ERROR = -32015;
inline constexpr int NOT_FOUND = -32023;
inline constexpr int FORBIDDEN = -32024;
inline constexpr int RESOURCE_EXHAUSTED = -32025;
inline constexpr int UNSUPPORTED = -32026;
// data.reason values of a CallbackEndpointError.
inline constexpr std::array<std::string_view, 6> CALLBACK_REASONS = {
    "connection_refused", "timeout", "tls_error", "http_4xx", "http_5xx", "challenge_failed"};
// What a Tailscale peer hears about any failed verification, and the lastError of any failed
// try to deliver to a peer's callback.
inline constexpr std::string_view PEER_VERIFICATION_FAILED = "challenge_failed";
inline constexpr std::string_view PEER_DELIVERY_FAILED = "delivery_failed";

// The job modes a subscription may filter on.
inline constexpr std::array<std::string_view, 6> MODES = {"dictation", "command",     "transform",
                                                          "action",    "walkthrough", "file"};

// "<env_prefix>MCP_EVENT_SECRET_" (FIORIVOCE_MCP_EVENT_SECRET_).
std::string secret_name_prefix();
// Everyone holding the MCP token is the same principal, the user's agents:
// "<data_dir_name in lower case>-mcp-token" ("fiorivoce-mcp-token").
std::string principal();
// OpenWhisper's principal, for comparing ids recorded from it.
std::string legacy_principal();
// "MCP Events is turned off in <product> Settings > API & MCP."
std::string off_message();

// A JSON-RPC error of an events/* request; the message names no internals.
class EventsError : public std::runtime_error {
public:
    EventsError(int code, const std::string& message, std::optional<Json> data = std::nullopt)
        : std::runtime_error(message), code_(code), data_(std::move(data)) {}
    int code() const noexcept { return code_; }
    const std::optional<Json>& data() const noexcept { return data_; }

private:
    int code_;
    std::optional<Json> data_;
};

// _too_many: a new subscription would pass the cap of `max`.
EventsError too_many(std::size_t max);
// _callback_failed: CALLBACK_ENDPOINT_ERROR with data.reason.
EventsError callback_failed(std::string_view reason);
// The Python exception class an error stands for, for log lines that name only a type.
std::string error_name(const std::exception& error);
// bytes.decode("utf-8") succeeds: no overlong forms, no surrogates, nothing above U+10FFFF.
bool decodes_as_utf8(std::string_view bytes);

// PAYLOAD_SCHEMAS: the `data` of each public event type, by type.
const Json& payload_schemas();
// JSON Schema of the `arguments` a subscription to `name` accepts.
Json input_schema(std::string_view name);
// events/list entries for every public event type, in catalog order.
Json descriptors();
// `arguments` checked against input_schema(name): {} for null, action_id before modes, modes
// sorted. Throws EventsError.
Json normalize_arguments(std::string_view name, const Json& arguments);
// _matches: whether an event's data passes a subscription's filters.
bool matches(const Json& arguments, const Json& data);
// _event_name: a public event type. Throws EventsError.
std::string event_name(const Json& value);

// `url` when it may receive deliveries: https anywhere, http only to this computer. Throws
// EventsError with validate_url's or urlsplit's reason.
std::string validate_callback_url(const Json& url);
// _is_loopback: localhost, or a loopback address.
bool is_loopback_host(std::string_view host);

// Where a callback URL leads, and for Public the address to connect to.
enum class Destination { Public, Private, Unresolved };
struct DestinationResult {
    Destination destination = Destination::Private;
    std::string address; // set for Public
    friend bool operator==(const DestinationResult&, const DestinationResult&) = default;
};
// How names are looked up and which addresses are this computer's. Tests replace both.
// Affinity: thread-safe; blocking.
struct Network {
    // socket.getaddrinfo(host, port) for TCP, each address without its scope; an error when the
    // name does not resolve.
    std::function<std::expected<std::vector<net::IpAddress>, std::string>(std::string_view host)>
        resolve;
    // _is_this_computer.
    std::function<bool(const net::IpAddress&)> is_this_computer;
};
// net::resolve_host and is_this_computer. A lookup ends with an error ("cancelled") once
// `cancel` is: the app passes its root token, which Quit cancels, since getaddrinfo itself has
// no timeout and a lookup waited for on a request thread would otherwise hold Quit.
Network system_network(CancelToken cancel = CancelToken());
// An address a connection from this computer to stays on it: Tailscale's service address
// (100.100.100.100, fd7a:115c:a1e0::53), which each computer's own Tailscale answers, or one of
// its own interfaces' (it can be bound), its Tailscale address included.
bool is_this_computer(const net::IpAddress& address);
// _destination: Private is this computer or a local network (a loopback, private, link-local or
// other non-global address, an address of this computer, a local name, or a name resolving to
// any of those); other Tailscale addresses are the peer's own network and count as Public. A
// name is looked up once: the request goes to the first address returned.
DestinationResult destination(std::string_view url, const Network& network);

// A client-chosen whsec_ secret with 24 to 64 bytes behind it. Throws EventsError.
std::string validate_secret(const Json& secret);
// (url, secret) of a `delivery` object; the secret is "" when unsubscribing. Throws EventsError.
std::pair<std::string, std::string> delivery(const Json& value, bool subscribing);
// The lifetime granted for a requested ttlMs: never more than asked except to reach the minimum,
// and the longest finite grant for null, so refreshBefore is never null. Throws EventsError.
std::int64_t granted_ttl_ms(const Json& params);
// secret_name_prefix() + the subscription's id.
std::string secret_name(std::string_view subscription);
// _iso: UTC "YYYY-MM-DDTHH:MM:SSZ" of whole Unix seconds (any fraction cut off).
std::string iso(double unix_seconds);
// _parse_iso: strptime(value, "%Y-%m-%dT%H:%M:%SZ") as Unix seconds; nullopt for anything else.
std::optional<std::int64_t> parse_iso(const Json& value);

class CallbackVerifier;
class Deliverer;
class Registry;
struct Job;

} // namespace events_ext

// "sub_" and the first 32 hex digits of the SHA-256 of the canonical JSON of {principal, url,
// name, arguments}: deterministic, so subscribing again is a refresh.
std::string subscription_id(std::string_view principal, std::string_view url, std::string_view name,
                            const Json& arguments);

// What tests change; the defaults are the app's.
struct McpEventsOptions {
    std::vector<double> retry_delays_s{events_ext::RETRY_DELAYS_S.begin(),
                                       events_ext::RETRY_DELAYS_S.end()};
    std::size_t max_subscriptions = events_ext::MAX_SUBSCRIPTIONS;
    events_ext::Network network = events_ext::system_network();
    // Replaces destination() for a peer's callback (a test stands a loopback receiver in for a
    // server on the internet).
    std::function<events_ext::DestinationResult(std::string_view url)> destination;
    std::string principal = events_ext::principal();
    // Run on a delivery worker before and after each try (tests hold and count tries).
    std::function<void()> before_try;
    std::function<void()> after_try;
};

// Affinity: thread-safe. The events/* methods block on verification (up to 10 s), the file and
// the credential store, on the caller's thread, which is never the GUI thread. Deliveries run on
// worker threads of their own; the bus's dispatch thread only matches and queues.
class McpEvents {
public:
    // `registry_file` is <data>/mcp_event_subscriptions.json, read at once. Everything must
    // outlive this object.
    McpEvents(SettingsStore& settings, CredentialStore& credentials, automation::EventBus& bus,
              std::filesystem::path registry_file, std::shared_ptr<http::Client> client,
              const Clock& clock, McpEventsOptions options = {});
    // Stops delivering and waits for the threads it started.
    ~McpEvents();
    McpEvents(const McpEvents&) = delete;
    McpEvents& operator=(const McpEvents&) = delete;

    // Serves events/list, events/subscribe and events/unsubscribe on `server` (both wires) and
    // advertises capabilities.events while MCP Events is on, read on each discovery. Called for
    // each listener's server (McpRuntime::add_protocol); this object must outlive them.
    void install(McpServer& server);

    // Delivers events to saved subscriptions while MCP Events is on; idempotent. Expired
    // subscriptions are forgotten on a worker thread.
    void start_delivery();
    // Stops delivering: queued deliveries are dropped, one being sent is cancelled, and the
    // subscriptions are kept. Waits for the delivery threads.
    void stop_delivery();
    // The MCP Events switch changed.
    void reload();
    // Active subscriptions agents made (Settings shows it); never waits on storage. Expired ones
    // are forgotten, secrets included, on a worker thread.
    std::size_t subscription_count() const;
    // Forgets every subscription and its secret; how many were active. Blocks on the credential
    // store and the file: never on the GUI thread.
    std::size_t clear_subscriptions();

    // events/list, events/subscribe and events/unsubscribe: `params` are the request's own
    // (without _meta), `saved` the saved settings, `origin` ORIGIN_LOCAL or ORIGIN_TAILSCALE (a
    // refresh takes the newer request's). Throw EventsError.
    Json list_events(const Json& params, const Json& saved, std::string_view origin);
    Json subscribe(const Json& params, const Json& saved, std::string_view origin);
    Json unsubscribe(const Json& params, const Json& saved, std::string_view origin);

    // Whether events are being taken from the bus (started, and MCP Events on when last synced).
    bool delivering() const;
    // The saved subscriptions (tests read and seed them).
    events_ext::Registry& registry() const;
    // Deliveries waiting for a worker.
    std::size_t pending() const;

private:
    using Method = Json (McpEvents::*)(const Json&, const Json&, std::string_view);

    Json load_settings() const;
    bool delivery_allowed(const Json& saved, std::string_view origin) const;
    void sync();
    void on_event(const automation::Event& event);
    std::optional<double> deliver(const events_ext::Job& job, const CancelToken& cancel);
    Json handle(const McpRequest& request, Method method);
    events_ext::DestinationResult peer_destination(std::string_view url) const;
    std::string require_reachable_by_peer(std::string_view url) const;

    SettingsStore& settings_;
    CredentialStore& credentials_;
    automation::EventBus& bus_;
    std::shared_ptr<http::Client> client_;
    const Clock& clock_;
    McpEventsOptions options_;
    std::unique_ptr<events_ext::Registry> registry_;
    std::unique_ptr<events_ext::CallbackVerifier> verifier_;

    // Guards the four members below; never held across a wait for another thread.
    mutable std::mutex state_mutex_;
    bool started_ = false;
    std::shared_ptr<automation::EventSubscription> bus_subscription_;
    std::shared_ptr<CallbackGuard> bus_guard_;
    std::shared_ptr<events_ext::Deliverer> deliverer_;
};

} // namespace fv::agentapi
