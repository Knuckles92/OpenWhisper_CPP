#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "base/cancel.h"
#include "base/clock.h"
#include "http/client.h"

// One POST to a webhook, a text filter or an action's address, and how its answer is told
// (services/automation/webhooks.py _exchange and its helpers): webhooks and filters share it.
// Redirects are never followed; loopback and private addresses skip proxies. Errors are fixed
// copy that never carries the URL, the request or the reply. Every function is thread-safe.
namespace fv::automation::hook_http {

// Per attempt, in seconds: connecting, sending and waiting for the answer each.
inline constexpr double TIMEOUT_S = 5.0;
inline constexpr std::string_view CONTENT_TYPE = "application/json";

// What one exchange came to (webhooks._Result).
// Affinity: value type.
struct Result {
    bool ok = false;
    std::optional<int> status;
    int elapsed_ms = 0;
    std::string error;
    // A network error, a timeout, 408, 429 or 5xx: worth trying again.
    bool retryable = false;
    // Seconds a retryable answer's Retry-After asked for.
    std::optional<double> retry_after;
    // The 2xx reply, up to the read limit; empty when none was read.
    std::string body;

    friend bool operator==(const Result&, const Result&) = default;
};

// _iso_now: UTC with milliseconds and "Z" ("2026-11-16T09:30:00.123Z").
std::string iso_now(const Clock& clock = system_clock());

// _retry_after: the seconds a Retry-After header asks for, as a number of seconds (Python's
// float(), at least 0, nullopt when not finite) or an HTTP date (email.utils, compared with
// `now`, at least 0). nullopt for a missing, empty or unreadable value.
std::optional<double> retry_after(std::optional<std::string_view> value,
                                  std::chrono::system_clock::time_point now);

// no_answer_copy: "No answer within {round(timeout_s, 3):g} s".
std::string no_answer_copy(double timeout_s);

// _transport_copy of the httpx error a failed transfer stands for: "Couldn't connect", "The
// proxy refused the request", "The server closed the connection" or "The connection broke".
std::string transport_copy(const http::Error& error);

// _is_private: hooks::url_is_private.
bool is_private(std::string_view url);

// _exchange: POSTs `body` once with `headers` (and User-Agent: http::user_agent()) and tells the
// answer; never throws. A 2xx reply is read up to `read_limit` bytes (0 reads none; more is "The
// reply was too large"), and reading stops at `deadline`. A 3xx is "HTTP {s}: redirects aren't
// followed"; network errors, timeouts, 408, 429 and 5xx are retryable, the last three with their
// Retry-After. `cancel` ends the transfer (callers that abandon a request).
Result exchange(http::Client& client, std::string_view url, std::string body,
                const http::Headers& headers, double timeout_s, std::size_t read_limit = 0,
                std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt,
                const CancelToken& cancel = {});

} // namespace fv::automation::hook_http
