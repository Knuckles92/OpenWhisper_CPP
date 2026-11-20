#pragma once

#include <compare>
#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/json.h"
#include "http/client.h"

// services/typesafe.py's transport (186-370): idle keep-alive connections per origin, shared by
// every judge of the process, and the POST that uses them. A connection here is an http::Client
// of its own (one curl share, so one live connection at a time is its whole cache); a request
// borrows it and nothing else uses it until it is handed back. Blocking; never on the GUI thread.
namespace fv::llm::detail {

// Idle connections kept per origin; meeting mode judges from a few threads.
inline constexpr std::size_t kPoolMaxIdle = 4;
// How long an idle connection stays eligible for reuse (seconds). A NAT or firewall that
// silently forgot it would stall the next judgment until its timeout.
inline constexpr double kPoolIdleS = 115.0;
// A fresh connection's TCP and TLS handshake alone takes about 100 ms, so a resend with less
// budget than this (seconds) could not be answered in time.
inline constexpr double kFreshAttemptMinS = 0.1;

// _reused_wait_s: how long a reused connection may stay silent before it is given up: half the
// budget, at least a second, never more than the budget (seconds).
double reused_wait_s(double timeout_s);

// (scheme, host, port) of an endpoint.
struct Origin {
    std::string scheme;
    std::string host;
    int port = 0;
    auto operator<=>(const Origin&) const = default;
};

// A new connection: an http::Client with a connection cache of its own.
using ConnectionFactory = std::function<std::shared_ptr<http::Client>()>;
// The proxy urllib would use for a URL, nullopt to connect directly. May throw.
using ProxyLookup = std::function<std::optional<std::string>(std::string_view url)>;

// _KeepAlivePool. Affinity: thread-safe; the lock guards the idle lists, never a round trip.
class KeepAlivePool {
public:
    struct Lease {
        std::shared_ptr<http::Client> connection;
        bool reused = false;
    };

    // `clock` (system_clock() when null) must outlive the pool.
    explicit KeepAlivePool(ConnectionFactory factory, std::size_t max_idle = kPoolMaxIdle,
                           double idle_s = kPoolIdleS, const Clock* clock = nullptr);

    // The most recently released idle connection that is still within the idle window, else a
    // new one. The connection itself notices a peer that closed it while idle and reconnects.
    Lease acquire(const Origin& origin);
    // Keeps the connection for reuse, or closes it when `max_idle` are already idle.
    void release(const Origin& origin, std::shared_ptr<http::Client> connection);
    // Closes every idle connection to `origin`, e.g. after the server restarted.
    void discard(const Origin& origin);

    // Idle connections over every origin.
    std::size_t idle() const;
    // Connections the factory made, and discard() calls: what tests count.
    std::size_t opened() const;
    std::size_t discards() const;

private:
    double now() const;

    ConnectionFactory factory_;
    std::size_t max_idle_;
    double idle_s_;
    const Clock& clock_;
    mutable std::mutex mutex_;
    std::map<Origin, std::vector<std::pair<std::shared_ptr<http::Client>, double>>> idle_;
    std::size_t opened_ = 0;
    std::size_t discards_ = 0;
};

// _proxied: true when the system routes `url` through a proxy, read per request; true as well
// when that cannot be told, so the one-shot request applies the proxy rules itself.
bool proxied(std::string_view url, const ProxyLookup& lookup);

// (status, body): the body only for a 2xx answer, because an error body may echo the request.
struct PostReply {
    int status = 0;
    std::string text;
};

// _http_post: POST `payload` as JSON to `endpoint` with a bearer key over a pooled connection;
// a proxied URL, or one that is not http(s), goes out once through `one_shot` instead. A request
// dropped or left unanswered on a reused connection is sent once more on a fresh one within what
// is left of `timeout_s` (seconds). `extra` headers never replace the four the request sets.
std::expected<PostReply, http::Error>
http_post(KeepAlivePool& pool, http::Client& one_shot, const ProxyLookup& lookup,
          const Json& payload, double timeout_s, std::string_view api_key,
          std::string_view endpoint, const http::Headers& extra, const CancelToken& cancel = {});

// The class of the exception http.client and urllib raise for a transport failure
// ("TimeoutError", "ConnectionRefusedError", "RemoteDisconnected", "gaierror", "SSLError", ...).
std::string python_exception_name(const http::Error& error);

} // namespace fv::llm::detail
