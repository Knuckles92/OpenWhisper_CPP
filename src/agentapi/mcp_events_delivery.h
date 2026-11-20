#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "agentapi/mcp_events.h"
#include "base/cancel.h"
#include "base/clock.h"
#include "http/client.h"

// Sending for MCP Events (events_ext.py _Reply to _deliver): one POST that never follows a
// redirect, never uses a proxy and never logs its URL; Standard Webhooks headers with one v1
// signature per key; the callback's verification challenge; and the workers that send due
// deliveries, retries waiting in the same queue.
namespace fv::automation {
struct Event;
}

namespace fv::agentapi::events_ext {

// What one POST came to. Affinity: value type.
struct Reply {
    std::optional<int> status;
    std::string body; // at most MAX_REPLY_BYTES
    // "" for 2xx, else a fixed category, never the URL or the answer: payload_too_large,
    // redirect, http_4xx, http_5xx, timeout, tls_error, connection_refused.
    std::string category;
    std::optional<double> retry_after;

    bool ok() const { return status && *status >= 200 && *status < 300; }
    // A network error, 408, 425, 429 or 5xx.
    bool retryable() const {
        return !status || *status == 408 || *status == 425 || *status == 429 || *status >= 500;
    }
    friend bool operator==(const Reply&, const Reply&) = default;
};

// _status_category of an HTTP status.
std::string status_category(int status);
// _retry_after: float(value) when it is a number of seconds, at least 0 (inf included); nullopt
// for a missing, empty or unreadable value.
std::optional<double> retry_after(std::optional<std::string_view> value);

// _post: one POST of `body` with `headers` and User-Agent: http::user_agent(); never follows a
// redirect, never uses a proxy, never logs the request, never throws. `connect_to` is the
// address to connect to instead of looking up the URL's host; the Host header and the TLS
// server name stay that host, and the certificate is still checked against it. `timeout_s`
// bounds connecting and each wait for data.
Reply post(http::Client& client, std::string_view url, const std::string& body,
           const http::Headers& headers, double timeout_s,
           const std::optional<std::string>& connect_to = std::nullopt,
           const CancelToken& cancel = {});

// Standard Webhooks headers, one v1 signature per key (rotation), then content-type and
// x-mcp-subscription-id. `keys` is not empty; `timestamp` in Unix seconds.
http::Headers signed_headers(const std::vector<std::string>& keys, std::string_view msg_id,
                             std::string_view body, std::string_view subscription_id,
                             std::int64_t timestamp);

// _event_body: ASCII compact JSON {"eventId","name","timestamp","data","cursor":null}, so
// whatever slipped into the event's text, the body is built.
std::string event_body(const automation::Event& event);

// Proves a callback wants our events before a subscription activates: a signed
// {"type":"verification","challenge":<nonce>} POST that must answer 2xx with {"challenge":
// <nonce>}. A URL that passed is not challenged again for VERIFY_CACHE_S.
// Affinity: thread-safe; verify() blocks.
class CallbackVerifier {
public:
    CallbackVerifier(http::Client& client, const Clock& clock);

    // Throws EventsError(CALLBACK_ENDPOINT_ERROR) with data.reason. `peer_address` is set for a
    // Tailscale peer's callback: the checked address to connect to, and every failure then
    // reads alike (challenge_failed).
    void verify(std::string_view subscription_id, const std::string& url, const std::string& secret,
                const std::optional<std::string>& peer_address, const CancelToken& cancel = {});

private:
    http::Client& client_;
    const Clock& clock_;
    std::mutex mutex_;
    std::map<std::string, std::chrono::steady_clock::time_point, std::less<>> verified_;
};

// One try of one event to one subscription; ordered by (due, order).
struct Job {
    std::chrono::steady_clock::time_point due;
    std::uint64_t order = 0;
    std::string subscription_id;
    std::string event_id;
    std::string event_type;
    std::shared_ptr<const std::string> body;
    int attempt = 1;
};

// A few worker threads sending due jobs; retries wait in the same queue. Workers start as jobs
// come, up to WORKERS.
// Affinity: thread-safe.
class Deliverer {
public:
    // One try: the delay in seconds before the next one, or nullopt. `cancel` is cancelled when
    // the deliverer closes. An exception is logged by type and ends the job.
    using Deliver = std::function<std::optional<double>(const Job&, const CancelToken&)>;

    Deliverer(Deliver deliver, const Clock& clock, std::size_t max_queued = MAX_QUEUED,
              std::size_t workers = WORKERS);
    // close()
    ~Deliverer();
    Deliverer(const Deliverer&) = delete;
    Deliverer& operator=(const Deliverer&) = delete;

    // Queues a try `delay_s` from now; false when closed or MAX_QUEUED jobs are waiting.
    bool push(std::string subscription_id, std::string event_id, std::string event_type,
              std::shared_ptr<const std::string> body, int attempt = 1, double delay_s = 0.0);
    std::size_t pending() const;
    // Drops what is queued, cancels the tries being sent and waits for the workers. Not from a
    // worker.
    void close();

private:
    struct Later {
        bool operator()(const Job& a, const Job& b) const {
            return a.due != b.due ? a.due > b.due : a.order > b.order;
        }
    };

    std::optional<Job> next();
    void run();

    Deliver deliver_;
    const Clock& clock_;
    std::size_t max_queued_;
    std::size_t workers_;
    CancelSource cancel_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::priority_queue<Job, std::vector<Job>, Later> heap_;
    std::uint64_t order_ = 0;
    std::vector<std::thread> threads_;
    bool closed_ = false;
};

} // namespace fv::agentapi::events_ext
