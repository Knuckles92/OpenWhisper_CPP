#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/json.h"
#include "http/client.h"
#include "llm/credentials.h"
#include "llm/profiles.h"
#include "pyport/errors.h"

// services/text_llm.py create_openai_client (672-697) and the parts of the `openai` SDK's
// request loop OpenWhisper relies on (_base_client.py 753-857, 1040-1167): a bearer-key JSON
// client for an OpenAI-compatible endpoint with the SDK's retry rules. Blocking; never on the GUI
// thread. No key, header value or body reaches a log.
namespace fv::llm {

// openai._constants.
inline constexpr std::string_view kOpenAiDefaultBaseUrl = "https://api.openai.com/v1";
inline constexpr int kSdkDefaultMaxRetries = 2;
inline constexpr double kSdkDefaultTimeoutS = 600.0;
inline constexpr double kSdkInitialRetryDelayS = 0.5;
inline constexpr double kSdkMaxRetryDelayS = 8.0;
// A server asking for a longer wait than this is not retried at all.
inline constexpr double kSdkMaxRetryAfterS = 120.0;

// The SDK's APIError family: `kind` is the Python class ("BadRequestError",
// "AuthenticationError", "PermissionDeniedError", "NotFoundError", "ConflictError",
// "UnprocessableEntityError", "RateLimitError", "InternalServerError", "APIStatusError",
// "APITimeoutError", "APIConnectionError"), `status` the HTTP status (0 without an answer), and
// what() str(exc): "Error code: 401 - {...}", "Request timed out.", "Connection error.".
class ApiError : public py::PyError {
public:
    ApiError(std::string_view kind, int status, const std::string& message)
        : py::PyError(kind, message), status(status), kind(kind) {}

    int status;
    std::string kind;
};

// The caller's cancel token ended the request (the SDK has no such outcome: OpenWhisper leaves
// the request to finish on its own thread).
class RequestCanceled : public std::runtime_error {
public:
    RequestCanceled() : std::runtime_error("canceled") {}
};

// Per-request options (the SDK's `timeout=`, `extra_headers=` and `max_retries` of
// with_options for one call).
struct RequestOptions {
    std::optional<double> timeout_s; // seconds per attempt; the client's when unset
    std::optional<int> max_retries;  // the client's when unset
    http::Headers headers;           // replace the client's of the same name (any case)
};

// What the client waits with between attempts, and its sources of jitter and wall time. Tests
// replace them; the defaults sleep on the cancel token, draw from a Mersenne twister and read
// the system clock.
struct RetryHooks {
    std::function<void(std::chrono::milliseconds, const CancelToken&)> sleep;
    std::function<double()> random; // [0, 1)
    const Clock* clock = nullptr;
};

// Affinity: thread-safe once built; copies share the http::Client (one connection pool).
class OpenAiCompatClient {
public:
    // `base_url` empty means OpenAI's; `timeout_s` bounds connecting and each wait for bytes, as
    // httpx's Timeout(timeout) does.
    OpenAiCompatClient(std::shared_ptr<http::Client> http, std::string base_url,
                       std::string api_key, http::Headers default_headers = {},
                       double timeout_s = kSdkDefaultTimeoutS,
                       int max_retries = kSdkDefaultMaxRetries);

    const std::string& base_url() const { return base_url_; }
    double timeout_s() const { return timeout_s_; }
    int max_retries() const { return max_retries_; }
    const http::Headers& default_headers() const { return default_headers_; }
    const std::shared_ptr<http::Client>& http_client() const { return http_; }

    // A copy with other limits that shares this client's connection pool.
    OpenAiCompatClient with_options(std::optional<double> timeout_s = std::nullopt,
                                    std::optional<int> max_retries = std::nullopt) const;
    // A copy with this client's URL, key, headers, limits and hooks that sends through `http`
    // (stream_generate's event-stream transport).
    OpenAiCompatClient with_http_client(std::shared_ptr<http::Client> http) const;

    void set_hooks(RetryHooks hooks);

    // POST {base}/{path} with a JSON body, or GET it, with the SDK's retries; the parsed answer.
    // A 2xx answer whose Content-Type is not JSON is its text when it does not parse (the SDK
    // hands back response.text). Throws ApiError, RequestCanceled, or py::JsonError for a JSON
    // answer that does not parse.
    Json post_json(std::string_view path, const Json& body, const RequestOptions& options = {},
                   const CancelToken& cancel = {}) const;
    Json get_json(std::string_view path, const RequestOptions& options = {},
                  const CancelToken& cancel = {}) const;

    // The wait before the attempt after `retries_taken` failed ones: the server's retry-after-ms
    // or retry-after (seconds or an HTTP date) when 0 < value <= 120 s, else
    // min(0.5 * 2^retries_taken, 8) s times 1 - 0.25 * random().
    std::chrono::milliseconds retry_delay(int retries_taken,
                                          const http::Headers& response_headers) const;
    // _should_retry: never when the server asks for more than 120 s; x-should-retry "true" or
    // "false" decides; else 408, 409, 429 and 5xx.
    bool should_retry(const http::Response& response) const;

    // The URL of `path` under the base URL, as the SDK joins them.
    std::string url_for(std::string_view path) const;

    // The hooks' clock: what an HTTP-date retry-after is measured from.
    const Clock& clock() const;

private:
    Json send(const std::string& method, std::string_view path, const Json* body,
              const RequestOptions& options, const CancelToken& cancel) const;

    std::shared_ptr<http::Client> http_;
    std::string base_url_;
    std::string api_key_;
    http::Headers default_headers_;
    double timeout_s_;
    int max_retries_;
    std::shared_ptr<const RetryHooks> hooks_;
};

// _parse_retry_after_header: retry-after-ms, then retry-after as seconds, then as an HTTP date
// (seconds from `clock`'s now); nullopt when none parses.
std::optional<double> parse_retry_after(const http::Headers& headers, const Clock& clock);

// The SDK's retry rules, shared by OpenAiCompatClient and detail::sdk_send. The wait before the
// attempt after `retries_taken` failed ones: the server's retry-after-ms or retry-after (seconds
// or an HTTP date, measured from `clock`) when 0 < value <= 120 s, else
// min(0.5 * 2^retries_taken, 8) s times 1 - 0.25 * `random01`.
std::chrono::milliseconds sdk_retry_delay(int retries_taken, const http::Headers& response_headers,
                                          const Clock& clock, double random01);
// _should_retry: never when the server asks for more than 120 s; x-should-retry "true" or
// "false" decides; else 408, 409, 429 and 5xx.
bool sdk_should_retry(const http::Response& response, const Clock& clock);

// The SDK's status class for an HTTP error status.
std::string_view status_error_kind(int status);

// create_openai_client: the key given, else resolve_api_key's (through `keys`); the profile's
// URL (OpenAI's without one) and provider_headers(profile, session_id). `max_retries` unset keeps
// the SDK's two. Throws py::PyError("RuntimeError", "No API key found for <name> (set <var>)").
OpenAiCompatClient create_openai_client(std::shared_ptr<http::Client> http,
                                        const TextLLMProfile& profile, double timeout_s = 15.0,
                                        std::string_view api_key = {},
                                        std::string_view session_id = {},
                                        std::optional<int> max_retries = std::nullopt,
                                        KeySources* keys = nullptr);

} // namespace fv::llm
