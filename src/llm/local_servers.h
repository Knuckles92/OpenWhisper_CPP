#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/cancel.h"
#include "base/executor.h"
#include "base/json.h"
#include "http/client.h"
#include "llm/credentials.h"
#include "llm/model_catalog.h"
#include "llm/profiles.h"

// services/local_text_servers.py: what a local inference server (Ollama, vLLM, llama.cpp,
// LM Studio, LocalAI) says about itself and its models, so cleanup and meetings know a model's
// real limits and reasoning control and embedding, speech and rerank models stay out of a chat
// list; and which server answers an address. The describers are pure functions over the JSON a
// server answered; the probes run over http::Client, block, and never run on the GUI thread.
namespace fv::llm {

// The server origin and path prefix in front of its /v1 routes.
std::string server_root(std::string_view base_url);

// _spec: a chat spec from what a server reported. `context` 0 means it did not say; otherwise
// the context is clamped to [1024, 2000000] and the output budget is min(4096, context / 2).
// `effort` (cleanup level -> the server's reasoning_effort value, null where the model cannot
// take it) is set when the server takes reasoning_effort for the model.
TextModelSpec server_spec(std::int64_t context = 0, bool tools = true, bool reasoning = false,
                          const std::optional<Json>& effort = std::nullopt);
// A model a server says nothing about, judged by its name: nullopt for embedding, rerank,
// whisper and tts models.
std::optional<TextModelSpec> spec_by_name(std::string_view model);

// The specs of `model_ids` (nullopt: not a chat model). `rows` are the /v1/models entries by id.
// Each throws CatalogError (llm/catalog_client.h) with Python's AttributeError or TypeError where
// OpenWhisper's describer raises on an answer of the wrong shape; discovery then judges every
// model by name.
// Ollama: one POST /api/show answer (`show`) per model.
std::optional<TextModelSpec> describe_ollama_model(const Json& show);
std::optional<TextModelSpec> describe_vllm_model(const Json& row);
std::optional<TextModelSpec> describe_llama_cpp_model(const Json& row);
// LM Studio: the GET /api/v1/models answer, or with `legacy` the /api/v0/models one (before 0.4).
std::map<std::string, std::optional<TextModelSpec>>
describe_lm_studio(const std::vector<std::string>& model_ids, const Json& listing, bool legacy);
// LocalAI: the GET /v1/models/capabilities answer.
std::map<std::string, std::optional<TextModelSpec>>
describe_localai(const std::vector<std::string>& model_ids, const Json& listing);

// The chat models at `base_url` (the server's /v1 URL), sorted, remembering their specs for
// model_spec under that URL. `server` names the local server preset answering there, or nullopt
// to identify it first; `api_key` is sent as a Bearer token on every request, the identification
// probes included. A server that will not describe its
// models still lists them, judged by name. Throws CatalogError (llm/catalog_client.h) when the
// model list cannot be read: httpx's "Client error '404 Not Found' for url '...'" and the
// connection's or JSON parser's message.
std::vector<std::string> discover_chat_models(
    http::Client& client, std::string_view base_url, std::optional<std::string_view> server,
    std::optional<std::string_view> api_key,
    std::chrono::milliseconds timeout = std::chrono::seconds(5), const CancelToken& cancel = {});

// The local server preset answering `base_url`, or nullopt, from routes that need no key (apart
// from llama.cpp's, whose key refusal still names it). Order: LocalAI's well-known file, Ollama,
// llama.cpp, vLLM, LM Studio; a failed first probe means nothing answers. 0.6 s per request.
std::optional<std::string>
identify_server(http::Client& client, std::string_view base_url,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(600),
                const CancelToken& cancel = {});

// {base_url: server} for the loopback addresses among `base_urls` that answer, in their first
// order, without repeats. One probe per address runs on `executor`, and this waits for all of
// them; called from a task of `executor` itself, it probes one address after another instead. A
// probe the executor drops unrun (after its shutdown) finds nothing. Nothing retries.
std::vector<std::pair<std::string, std::string>>
detect_running_servers(http::Client& client, const std::vector<std::string>& base_urls,
                       Executor& executor, const CancelToken& cancel = {});

// text_llm.discover_server_models: a self-hosted profile's chat models, through its preset's
// describer or, for a custom endpoint, after identifying the server. An empty `key` is resolved
// as list_chat_models does; "dummy" is not sent. Throws CatalogError.
std::vector<std::string> discover_server_models(
    http::Client& client, const TextLLMProfile& profile, std::string_view key, KeySources* keys,
    std::chrono::milliseconds timeout = std::chrono::seconds(15), const CancelToken& cancel = {});

// Asks a server the user runs about its models once per process (3 s), so model_spec knows a
// self-hosted model's real context and reasoning control before the first request. Other
// endpoints are left alone; a server that will not say keeps the defaults.
void learn_server_models(http::Client& client, const TextLLMProfile& profile, std::string_view key,
                         KeySources* keys,
                         std::chrono::milliseconds timeout = std::chrono::seconds(3),
                         const CancelToken& cancel = {});

// snapshot_from_profile as OpenWhisper runs it: a server the user runs is asked about its models
// first (learn_server_models) when `model` is given.
TextLLMSnapshot snapshot_from_profile(const TextLLMProfile& profile, std::string_view model,
                                      http::Client& client, KeySources& keys);
// resolve_meeting_llm_endpoint as OpenWhisper runs it, learning the server first.
Json resolve_meeting_llm_endpoint(const Json& settings, http::Client& client, KeySources& keys);

} // namespace fv::llm
