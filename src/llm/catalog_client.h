#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "base/cancel.h"
#include "http/client.h"
#include "llm/credentials.h"
#include "llm/profiles.h"
#include "pyport/errors.h"

// services/text_llm.py's requests to a text-model endpoint (699-811): verifying a key and
// listing chat models. Blocking; never on the GUI thread. A provider's error body can echo the
// key back, so key verification never reports or logs one.
namespace fv::llm {

struct KeyCheck {
    bool ok = false;
    std::string detail; // one sentence to show beside the key
};

// The OpenAI SDK's waits between attempts. Tests replace the wait.
using RetryWait = std::function<void(std::chrono::milliseconds, const CancelToken&)>;

// One authenticated request with `key` (10 s timeouts and the SDK's two retries): OpenRouter's
// GET {base}/auth/key (its model catalog is public, so listing it proves nothing), else
// GET {base}/models. Only the status class is reported: "<host> accepted the key.", "<host>
// rejected the key (HTTP 401).", "<host> accepted the key but denied access (HTTP 403).",
// "<host> answered HTTP <n>.", "Couldn't reach <host>.", and for OpenCode's public catalog "Catalog
// reachable. OpenCode's public catalog cannot verify a key; a model request is required.".
// <host> is the base URL's host name, "api.openai.com" without a base URL, "the endpoint"
// without a host. An empty `key` is resolved through `keys` as the SDK client does ("dummy"
// for an endpoint that needs none); a required key that is missing (or no `keys`) is
// "Verification failed (RuntimeError).".
KeyCheck verify_api_key(http::Client& client, const TextLLMProfile& profile, std::string_view key,
                        const CancelToken& cancel, KeySources* keys = nullptr);
KeyCheck verify_api_key(http::Client& client, const TextLLMProfile& profile, std::string_view key,
                        const CancelToken& cancel, KeySources* keys, const RetryWait& wait);

// The OpenAI model ids usable with chat: "gpt-", "chatgpt-", "o1", "o3", "o4" ids without audio,
// realtime, tts, whisper, embedding, moderation, dall-e, transcribe, image, search or instruct.
std::vector<std::string> filter_openai_chat_models(const std::vector<std::string>& model_ids);

// A model list failure, worded as Python's exception str() is (shown as "Couldn't load models:
// <what()>"); type_name() is the Python exception class.
class CatalogError : public py::PyError {
public:
    CatalogError(std::string_view type_name, const std::string& message)
        : py::PyError(type_name, message) {}
};

// The endpoint's chat model ids. A server the user runs is asked what each model is (which also
// records its limits for model_spec); anywhere else GET {base}/models (15 s, the SDK's retries)
// with OpenRouter's server-side `sort` when it is not "alphabetical", kept in the server's
// order; otherwise OpenAI's and Groq's non-chat models are dropped and the ids sorted. An empty
// `key` is resolved as verify_api_key does. Throws CatalogError: "No API key found for <name>
// (set <variable>)" (RuntimeError), the SDK's "Error code: <n> - ...", "Connection error.",
// "Request timed out.", or the local server's errors (llm/local_servers.h).
std::vector<std::string> list_chat_models(http::Client& client, const TextLLMProfile& profile,
                                          std::string_view key, std::string_view sort,
                                          const CancelToken& cancel, KeySources* keys = nullptr);
std::vector<std::string> list_chat_models(http::Client& client, const TextLLMProfile& profile,
                                          std::string_view key, std::string_view sort,
                                          const CancelToken& cancel, KeySources* keys,
                                          const RetryWait& wait);

} // namespace fv::llm
