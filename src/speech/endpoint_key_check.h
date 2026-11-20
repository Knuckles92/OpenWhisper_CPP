#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/env.h"
#include "http/client.h"
#include "speech/endpoints.h"

namespace fv::speech {

struct KeyCheck {
    bool ok = false;
    std::string detail; // one sentence to show beside the key
};

// The OpenAI SDK's waits between attempts: retry-after-ms or retry-after (seconds, or an HTTP date
// counted from the clock's now) when the server asks for 0 to 60 s, else 0.5 s doubling to at most
// 8 s, less up to a quarter of jitter. Tests replace the wait.
using RetryWait = std::function<void(std::chrono::milliseconds, const CancelToken&)>;

struct KeyCheckOptions {
    // resolve_api_key, for an empty `key` (create_openai_client's `api_key or resolve_api_key`):
    // speech::api_key over the saved key, the environment and .env. nullopt or "" means the key
    // is missing. Unset: the placeholder for a server that needs no key, else missing.
    std::function<std::optional<std::string>()> stored_key;
    // Where the SDK reads OPENAI_BASE_URL for an endpoint without a base URL. nullptr:
    // system_env().
    const EnvReader* env = nullptr;
    RetryWait wait;               // unset: waits on the cancel token
    const Clock* clock = nullptr; // an HTTP-date Retry-After's now; nullptr: system_clock()
};

// text_llm.verify_api_key's GET {base}/models branch: one authenticated request with `key`
// (10 s timeouts, the SDK's two retries for connection failures, 408, 409, 429 and 5xx) and only
// the HTTP status class reported, since provider error bodies can echo the credential back:
// "<host> accepted the key.", "<host> rejected the key (HTTP 401).", "<host> accepted the key but
// denied access (HTTP 403).", "<host> answered HTTP <n>.", "Couldn't reach <host>.". <host> is
// api.openai.com for OpenAI (also when OPENAI_BASE_URL sends the request elsewhere, as Python
// names it), else the base URL's host name. An empty `key` is the stored one; when that is
// missing too it fails as Python does ("Verification failed (RuntimeError)."). Blocking; never
// on the GUI thread.
KeyCheck verify_endpoint_key(http::Client& client, const SpeechEndpoint& endpoint,
                             std::string_view key, const CancelToken& cancel);
KeyCheck verify_endpoint_key(http::Client& client, const SpeechEndpoint& endpoint,
                             std::string_view key, const CancelToken& cancel,
                             const RetryWait& wait);
KeyCheck verify_endpoint_key(http::Client& client, const SpeechEndpoint& endpoint,
                             std::string_view key, const CancelToken& cancel,
                             const KeyCheckOptions& options);

} // namespace fv::speech
