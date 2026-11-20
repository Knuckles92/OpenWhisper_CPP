#include "speech/endpoint_key_check.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <random>

#include "base/log.h"
#include "pyport/email_date.h"
#include "pyport/number.h"
#include "speech/detail/url.h"

namespace fv::speech {

namespace {

constexpr log::Logger kLog{"services.text_llm"};

// verify_api_key's timeout, and the SDK's default retries and delays.
constexpr auto kVerifyTimeout = std::chrono::milliseconds(10'000);
constexpr int kMaxRetries = 2;
constexpr double kInitialRetryDelayS = 0.5;
constexpr double kMaxRetryDelayS = 8.0;

std::string host_of(const SpeechEndpoint& endpoint) {
    if (!endpoint.base_url) {
        return "api.openai.com";
    }
    try {
        return detail::urlsplit(*endpoint.base_url).hostname().value_or("the endpoint");
    } catch (const std::invalid_argument&) {
        return "the endpoint";
    }
}

// _parse_retry_after_header: retry-after-ms, then retry-after as seconds or as an HTTP date
// (parsedate_tz and mktime_tz, less time.time()).
std::optional<double> retry_after_s(const http::Headers& headers, const Clock& clock) {
    if (const auto ms = http::find_header(headers, "retry-after-ms")) {
        if (const auto value = py::py_float(*ms)) {
            return *value / 1000.0;
        }
    }
    if (const auto seconds = http::find_header(headers, "retry-after")) {
        if (const auto value = py::py_float(*seconds)) {
            return *value;
        }
        if (const auto when = py::parse_http_date(*seconds)) {
            const auto now = std::chrono::duration<double>(clock.utc().time_since_epoch());
            return static_cast<double>(*when) - now.count();
        }
    }
    return std::nullopt;
}

std::chrono::milliseconds retry_delay(int remaining, const http::Headers& headers,
                                      const Clock& clock) {
    if (const auto asked = retry_after_s(headers, clock); asked && *asked > 0 && *asked <= 60) {
        return std::chrono::milliseconds(static_cast<std::int64_t>(*asked * 1000.0));
    }
    const int retries = kMaxRetries - remaining;
    const double sleep = std::min(kInitialRetryDelayS * std::pow(2.0, retries), kMaxRetryDelayS);
    thread_local std::mt19937 random{std::random_device{}()};
    const double jitter = 1.0 - 0.25 * std::uniform_real_distribution<double>(0.0, 1.0)(random);
    return std::chrono::milliseconds(
        static_cast<std::int64_t>(std::max(0.0, sleep * jitter) * 1000.0));
}

bool should_retry(const http::Response& response) {
    if (const auto header = http::find_header(response.headers, "x-should-retry")) {
        if (*header == "true") {
            return true;
        }
        if (*header == "false") {
            return false;
        }
    }
    const int status = response.status;
    return status == 408 || status == 409 || status == 429 || status >= 500;
}

} // namespace

KeyCheck verify_endpoint_key(http::Client& client, const SpeechEndpoint& endpoint,
                             std::string_view key, const CancelToken& cancel) {
    return verify_endpoint_key(client, endpoint, key, cancel, KeyCheckOptions{});
}

KeyCheck verify_endpoint_key(http::Client& client, const SpeechEndpoint& endpoint,
                             std::string_view key, const CancelToken& cancel,
                             const RetryWait& wait) {
    KeyCheckOptions options;
    options.wait = wait;
    return verify_endpoint_key(client, endpoint, key, cancel, options);
}

KeyCheck verify_endpoint_key(http::Client& client, const SpeechEndpoint& endpoint,
                             std::string_view key, const CancelToken& cancel,
                             const KeyCheckOptions& options) {
    const Clock& clock = options.clock != nullptr ? *options.clock : system_clock();
    const RetryWait wait = options.wait
                               ? options.wait
                               : RetryWait([](std::chrono::milliseconds delay,
                                              const CancelToken& token) { token.wait_for(delay); });
    const std::string host = host_of(endpoint);
    std::string sent(key);
    if (sent.empty()) {
        if (options.stored_key) {
            sent = options.stored_key().value_or(std::string());
        } else if (endpoint.api_key_env.empty()) {
            sent = std::string(kAuthFreeApiKey);
        }
        if (sent.empty()) {
            // create_openai_client: "No API key found for <name> (set <variable>)".
            kLog.debug("Key verification failed: {}", "RuntimeError");
            return {false, "Verification failed (RuntimeError)."};
        }
    }
    http::Request request;
    if (endpoint.base_url) {
        request.url = *endpoint.base_url;
    } else {
        // The SDK's base_url=None: OPENAI_BASE_URL, else OpenAI's.
        const EnvReader& env = options.env != nullptr ? *options.env : system_env();
        const auto from_env = env.get("OPENAI_BASE_URL");
        request.url = from_env && !from_env->empty() ? *from_env : std::string(kOpenAiBaseUrl);
    }
    while (request.url.ends_with('/')) {
        request.url.pop_back();
    }
    request.url += "/models";
    request.headers.emplace_back("Authorization", "Bearer " + sent);
    request.headers.emplace_back("Accept", "application/json");
    request.connect_timeout = kVerifyTimeout;
    request.idle_timeout = kVerifyTimeout;
    request.follow_redirects = true;

    for (int remaining = kMaxRetries;; --remaining) {
        const auto response = client.send(request, cancel);
        if (!response) {
            if (response.error().kind == http::ErrorKind::Cancelled || cancel.cancelled()) {
                kLog.debug("Key verification failed: {}", "Cancelled");
                return {false, "Verification failed (Cancelled)."};
            }
            if (remaining > 0) {
                wait(retry_delay(remaining, {}, clock), cancel);
                continue;
            }
            return {false, std::format("Couldn't reach {}.", host)};
        }
        const int status = response->status;
        if (status >= 200 && status < 300) {
            return {true, std::format("{} accepted the key.", host)};
        }
        if (remaining > 0 && should_retry(*response)) {
            wait(retry_delay(remaining, response->headers, clock), cancel);
            continue;
        }
        if (status == 401) {
            return {false, std::format("{} rejected the key (HTTP 401).", host)};
        }
        if (status == 403) {
            return {false, std::format("{} accepted the key but denied access (HTTP 403).", host)};
        }
        return {false, std::format("{} answered HTTP {}.", host, status)};
    }
}

} // namespace fv::speech
