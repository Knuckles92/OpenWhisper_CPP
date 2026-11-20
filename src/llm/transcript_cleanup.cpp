#include "llm/transcript_cleanup.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <thread>

#include "base/ids.h"
#include "base/log.h"
#include "llm/catalog_client.h"
#include "llm/cleanup_settings.h"
#include "llm/detail/exception_kind.h"
#include "llm/generation.h"
#include "llm/local_servers.h"
#include "llm/thinking_levels.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::llm {

namespace {

constexpr log::Logger kLog{"services.transcript_cleanup"};

bool is_reasoning_level(std::string_view level) {
    return std::ranges::find(kThinkingOrder, level) != kThinkingOrder.end();
}

bool is_known_cleanup_id(std::string_view provider) {
    return std::ranges::find(kTranscriptCleanupProviders, provider) !=
               kTranscriptCleanupProviders.end() ||
           is_installed_cleanup_agent(provider);
}

// str(uuid.uuid4()).
std::string uuid4_text() {
    const std::string hex = uuid4_hex();
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
           hex.substr(16, 4) + "-" + hex.substr(20);
}

// _sent_reasoning: the saved level, or the nearest one the model accepts.
std::string sent_reasoning(const std::string& provider, const std::string& model,
                           const std::string& reasoning, const Json& settings) {
    return effective_level(reasoning, levels_for(provider, model, settings));
}

class TimedOut : public std::exception {};
class Canceled : public std::exception {};
class AgentUnavailable : public std::exception {};

} // namespace

// One request on its own thread. The waiter and the thread share it; whoever finishes last
// frees it.
struct TranscriptCleanup::Pending {
    explicit Pending(const CancelToken& cancel) : source(cancel) {}

    CancelSource source;
    std::mutex mutex;
    std::condition_variable done_changed;
    bool done = false;
    std::string result;
    std::chrono::steady_clock::time_point finished_at;
    std::exception_ptr error;
    std::thread thread;
};

std::string provider_env_key(std::string_view provider, const Json& settings) {
    if (is_installed_cleanup_agent(provider)) {
        return "";
    }
    if (const auto profile = get_profile(provider, settings)) {
        return profile->api_key_env;
    }
    if (provider == kOpenRouterProfileId) {
        return "OPENROUTER_API_KEY";
    }
    return "OPENAI_API_KEY";
}

std::optional<std::string> find_api_key(std::string_view provider, const Json& settings,
                                        KeySources& keys) {
    if (is_installed_cleanup_agent(provider)) {
        return std::nullopt;
    }
    if (const auto profile = get_profile(provider, settings)) {
        return resolve_api_key(*profile, keys);
    }
    const std::string env_key = provider_env_key(provider, settings);
    if (env_key.empty()) {
        return std::string(kAuthFreeApiKey);
    }
    return lookup_env_value(env_key, keys);
}

std::vector<std::string> list_cleanup_models(http::Client& client, std::string_view provider,
                                             std::optional<std::string_view> sort,
                                             const Json& settings, KeySources& keys,
                                             std::string_view api_key, const CancelToken& cancel) {
    const auto profile = get_profile(provider, settings);
    if (!profile) {
        throw CatalogError("RuntimeError",
                           "Unknown text model endpoint '" + std::string(provider) + "'");
    }
    return list_chat_models(client, *profile, api_key, sort.value_or(""), cancel, &keys);
}

AttemptBudget attempt_budget(double client_timeout_s, std::optional<double> timeout_s,
                             std::optional<double> deadline_s) {
    AttemptBudget budget;
    budget.attempt_timeout_s = client_timeout_s;
    budget.max_retries = kTranscriptCleanupMaxRetries;
    if (timeout_s) {
        budget.attempt_timeout_s = *timeout_s;
        budget.max_retries = kTranscriptBatchCleanupMaxRetries;
        budget.overridden = true;
    }
    budget.give_up_s =
        budget.attempt_timeout_s * (budget.max_retries + 1) + kRetryBackoffAllowanceS;
    if (deadline_s) {
        budget.give_up_s = *deadline_s;
    }
    return budget;
}

TranscriptCleanup::TranscriptCleanup(std::shared_ptr<http::Client> http, KeySources keys,
                                     SettingsReader settings, std::string_view provider,
                                     std::string_view model, std::string_view reasoning,
                                     bool defer_client, CleanupOptions options)
    : http_(std::move(http)), keys_(keys), settings_(std::move(settings)),
      options_(std::move(options)), timeout_s_(kTranscriptCleanupTimeoutS) {
    const Json snapshot = settings_ ? settings_() : Json::object();
    provider_ = normalize_provider(provider, snapshot);
    model_ =
        model.empty() ? default_transcript_cleanup_model(provider_, snapshot) : std::string(model);
    reasoning_ = is_reasoning_level(reasoning) ? std::string(reasoning) : std::string("off");
    if (is_installed_cleanup_agent(provider_)) {
        timeout_s_ = kAgentCleanupTimeoutS;
    } else if (options_.api_key && !options_.api_key->empty()) {
        api_key_ = options_.api_key;
    }
    // Deferred, the key is looked up by the first configure(), which rebuilds anyway: the
    // constructor may then run where the credential store must not be read.
    if (!defer_client) {
        std::lock_guard lock(mutex_);
        if (!api_key_) {
            api_key_ = find_api_key(provider_, snapshot, keys_);
        }
        initialize_client(snapshot);
    }
}

TranscriptCleanup::~TranscriptCleanup() {
    reap(true);
}

std::string TranscriptCleanup::normalize_provider(std::string_view provider,
                                                  const Json& settings) const {
    if (!provider.empty() && get_profile(provider, settings)) {
        return std::string(provider);
    }
    if (is_known_cleanup_id(provider)) {
        return std::string(provider);
    }
    return std::string(kTranscriptCleanupProvider);
}

void TranscriptCleanup::initialize_client(const Json& settings) {
    if (is_installed_cleanup_agent(provider_)) {
        api_key_.reset();
        client_.reset();
        connection_.reset();
        generation_ = 0;
        timeout_s_ = kAgentCleanupTimeoutS;
        return;
    }
    const auto profile = get_profile(provider_, settings);
    if (!profile) {
        // Only ids that resolve are kept, so this guards a default naming a deleted endpoint.
        // Cleanup is optional: stay quiet and unavailable.
        kLog.debug("No text model endpoint '{}'; transcript cleanup unavailable", provider_);
        client_.reset();
        connection_.reset();
        generation_ = 0;
        return;
    }
    if (!api_key_ || api_key_->empty()) {
        api_key_ = find_api_key(provider_, settings, keys_);
    }
    if (!api_key_ || api_key_->empty()) {
        kLog.debug("No {} API key; transcript cleanup unavailable", profile->api_key_env);
        client_.reset();
        connection_.reset();
        generation_ = 0;
        return;
    }
    try {
        timeout_s_ =
            profile->self_hosted() ? kSelfHostedCleanupTimeoutS : kTranscriptCleanupTimeoutS;
        auto client = options_.client_factory
                          ? options_.client_factory(*profile, timeout_s_, *api_key_)
                          : create_openai_client(http_, *profile, timeout_s_, *api_key_, {},
                                                 kTranscriptCleanupMaxRetries, &keys_);
        if (!options_.client_factory) {
            client.set_hooks(options_.retry_hooks);
        }
        client_ = std::move(client);
        connection_ = connection_fingerprint(*profile, keys_);
        generation_ = ++next_generation_;
        kLog.info("Transcript cleanup client initialized ({})", profile->id);
    } catch (const std::exception& error) {
        kLog.error_with(error, "Failed to initialize transcript cleanup client");
        client_.reset();
        connection_.reset();
        generation_ = 0;
    }
}

void TranscriptCleanup::configure(std::string_view provider, std::string_view model,
                                  std::optional<std::string_view> reasoning) {
    const Json settings = settings_ ? settings_() : Json::object();
    std::lock_guard lock(mutex_);
    const std::string normalized = normalize_provider(provider, settings);
    const auto profile = get_profile(normalized, settings);
    std::optional<ConnectionFingerprint> fingerprint;
    if (profile) {
        fingerprint = connection_fingerprint(*profile, keys_);
    }
    const bool needs_rebuild =
        normalized != provider_ || (fingerprint && fingerprint != connection_);
    if (needs_rebuild) {
        provider_ = normalized;
        api_key_ = find_api_key(normalized, settings, keys_);
        initialize_client(settings);
    }
    model_ = py::strip(model);
    if (reasoning && is_reasoning_level(*reasoning)) {
        reasoning_ = std::string(*reasoning);
    }
}

double TranscriptCleanup::attempt_timeout_s() const {
    std::lock_guard lock(mutex_);
    return timeout_s_;
}

bool TranscriptCleanup::is_available() const {
    std::string provider;
    {
        std::lock_guard lock(mutex_);
        if (!is_installed_cleanup_agent(provider_)) {
            return client_.has_value() && api_key_.has_value() && !model_.empty();
        }
        provider = provider_;
    }
    try {
        return options_.agent_available && options_.run_agent && settings_ &&
               cleanup_agent_authorized(settings_(), provider) &&
               options_.agent_available(provider);
    } catch (...) {
        return false;
    }
}

std::optional<std::string> TranscriptCleanup::last_error() const {
    std::lock_guard lock(mutex_);
    return last_error_;
}

std::string TranscriptCleanup::provider() const {
    std::lock_guard lock(mutex_);
    return provider_;
}

std::string TranscriptCleanup::model() const {
    std::lock_guard lock(mutex_);
    return model_;
}

std::string TranscriptCleanup::reasoning() const {
    std::lock_guard lock(mutex_);
    return reasoning_;
}

std::optional<std::string> TranscriptCleanup::api_key() const {
    std::lock_guard lock(mutex_);
    return api_key_;
}

std::uint64_t TranscriptCleanup::client_generation() const {
    std::lock_guard lock(mutex_);
    return generation_;
}

std::optional<ConnectionFingerprint> TranscriptCleanup::connection() const {
    std::lock_guard lock(mutex_);
    return connection_;
}

void TranscriptCleanup::set_last_error(std::optional<std::string> error) {
    std::lock_guard lock(mutex_);
    last_error_ = std::move(error);
}

void TranscriptCleanup::reap(bool all) {
    std::vector<std::shared_ptr<Pending>> joining;
    {
        std::lock_guard lock(pending_mutex_);
        auto split = std::partition(abandoned_.begin(), abandoned_.end(),
                                    [all](const std::shared_ptr<Pending>& pending) {
                                        if (all) {
                                            return false;
                                        }
                                        std::lock_guard done_lock(pending->mutex);
                                        return !pending->done;
                                    });
        joining.assign(split, abandoned_.end());
        abandoned_.erase(split, abandoned_.end());
    }
    for (const auto& pending : joining) {
        pending->source.cancel();
        if (pending->thread.joinable()) {
            pending->thread.join();
        }
    }
}

std::string TranscriptCleanup::cleanup(std::string_view text, std::string_view system_prompt,
                                       std::optional<double> timeout_s,
                                       std::optional<double> deadline_s,
                                       const CancelToken& cancel) {
    reap(false);
    if (text.empty() || py::strip(text).empty()) {
        set_last_error("empty input");
        return std::string(text);
    }
    if (cancel.cancelled()) {
        set_last_error(std::string(kCanceledReason));
        return std::string(text);
    }
    std::string provider;
    std::string model;
    std::string reasoning;
    std::string api_key;
    double client_timeout_s = 0.0;
    std::optional<OpenAiCompatClient> client;
    {
        std::lock_guard lock(mutex_);
        provider = provider_;
        model = model_;
        reasoning = reasoning_;
        api_key = api_key_.value_or("");
        client_timeout_s = timeout_s_;
        client = client_;
    }
    const bool use_agent = is_installed_cleanup_agent(provider);
    if (use_agent ? (!options_.agent_available || !options_.run_agent || !settings_)
                  : (!client || api_key.empty() || model.empty())) {
        set_last_error("cleanup unavailable");
        return std::string(text);
    }
    std::string prompt = py::strip(system_prompt);
    if (prompt.empty()) {
        prompt = options_.default_prompt;
    }
    const auto budget =
        use_agent ? AttemptBudget{client_timeout_s, 0,
                                  deadline_s.value_or(timeout_s.value_or(client_timeout_s)), false}
                  : attempt_budget(client_timeout_s, timeout_s, deadline_s);
    const Clock& clock = options_.clock != nullptr ? *options_.clock : system_clock();
    const auto give_up_at =
        clock.steady() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                             std::chrono::duration<double>(budget.give_up_s));

    std::function<std::string(const CancelToken&)> work;
    if (use_agent) {
        work = [read = settings_, available = options_.agent_available, run = options_.run_agent,
                provider, model, reasoning, input = std::string(text), prompt, budget, give_up_at,
                &clock](const CancelToken& token) {
            if (token.cancelled()) {
                throw Canceled();
            }
            if (!cleanup_agent_authorized(read(), provider)) {
                throw AgentUnavailable();
            }
            if (token.cancelled()) {
                throw Canceled();
            }
            if (!available(provider)) {
                throw AgentUnavailable();
            }
            const Json latest = read();
            if (!cleanup_agent_authorized(latest, provider)) {
                throw AgentUnavailable();
            }
            const std::string effort = sent_reasoning(provider, model, reasoning, latest);
            if (token.cancelled()) {
                throw Canceled();
            }
            if (clock.steady() >= give_up_at) {
                throw TimedOut();
            }
            auto cleaned = run({provider, model, input, prompt, effort, budget.give_up_s}, token);
            if (!cleaned) {
                throw std::runtime_error("Agent cleanup failed");
            }
            return std::move(*cleaned);
        };
    } else {
        const Json settings = settings_ ? settings_() : Json::object();
        const auto profile = get_profile(provider, settings);
        Json request_kwargs;
        if (profile) {
            request_kwargs = chat_request_options(
                *profile, sent_reasoning(provider, model, reasoning, settings));
        } else if (reasoning == "off") {
            request_kwargs = Json{{"temperature", 0}};
        } else if (provider == kOpenRouterProfileId) {
            request_kwargs = Json{{"extra_body", Json{{"reasoning", Json{{"effort", reasoning}}}}}};
        } else {
            request_kwargs = Json{{"reasoning_effort", reasoning}};
        }
        if (timeout_s) {
            request_kwargs["timeout"] = *timeout_s;
        }

        if (budget.overridden) {
            // A copy that shares the client's connection pool.
            client = client->with_options(std::nullopt, budget.max_retries);
        }
        Json messages = Json::array();
        messages.push_back(Json{{"role", "system"}, {"content", prompt}});
        messages.push_back(Json{{"role", "user"}, {"content", std::string(text)}});

        work = [client = std::move(*client), profile, settings, http = http_, keys = keys_,
                provider, model, reasoning, api_key, messages = std::move(messages),
                request_kwargs = std::move(request_kwargs)](const CancelToken& token) mutable {
            if (profile) {
                learn_server_models(*http, *profile, api_key, &keys, std::chrono::seconds(3),
                                    token);
            }
            GenerateRequest request;
            request.model = model;
            request.messages = std::move(messages);
            request.cancel = token;
            request.session_id = uuid4_text();
            request.reasoning_level = sent_reasoning(provider, model, reasoning, settings);
            request.options = std::move(request_kwargs);
            return generate(client, profile ? &*profile : nullptr, std::move(request)).text;
        };
    }

    auto pending = std::make_shared<Pending>(cancel);
    pending->thread = std::thread([pending, work = std::move(work), &clock] {
        try {
            pending->result = work(pending->source.token());
        } catch (...) {
            std::lock_guard lock(pending->mutex);
            pending->error = std::current_exception();
        }
        {
            std::lock_guard lock(pending->mutex);
            pending->finished_at = clock.steady();
            pending->done = true;
        }
        pending->done_changed.notify_all();
    });

    try {
        while (true) {
            if (cancel.cancelled()) {
                throw Canceled();
            }
            const auto remaining = give_up_at - clock.steady();
            if (remaining <= std::chrono::steady_clock::duration::zero()) {
                throw TimedOut();
            }
            const auto wait = std::min<std::chrono::steady_clock::duration>(
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(kCancelPoll),
                remaining);
            {
                std::unique_lock lock(pending->mutex);
                if (pending->done_changed.wait_for(lock, wait, [&] { return pending->done; })) {
                    if (cancel.cancelled()) {
                        throw Canceled();
                    }
                    if (use_agent && pending->finished_at >= give_up_at) {
                        throw TimedOut();
                    }
                    break;
                }
            }
        }
    } catch (const TimedOut&) {
        pending->source.cancel();
        {
            std::lock_guard lock(pending_mutex_);
            abandoned_.push_back(pending);
        }
        const std::string reason =
            "timed out after " + py::format_fixed(budget.give_up_s, 0) + " s";
        set_last_error(reason);
        kLog.warning("Transcript cleanup failed; using raw text ({})", "_CleanupTimedOut");
        return std::string(text);
    } catch (const Canceled&) {
        pending->source.cancel();
        {
            std::lock_guard lock(pending_mutex_);
            abandoned_.push_back(pending);
        }
        set_last_error(std::string(kCanceledReason));
        kLog.info("Transcript cleanup canceled; its response will be ignored");
        return std::string(text);
    }
    pending->thread.join();
    if (pending->error) {
        try {
            std::rethrow_exception(pending->error);
        } catch (const AgentUnavailable&) {
            set_last_error("cleanup unavailable");
        } catch (const Canceled&) {
            set_last_error(std::string(kCanceledReason));
        } catch (const TimedOut&) {
            set_last_error("timed out after " + py::format_fixed(budget.give_up_s, 0) + " s");
        } catch (const std::exception& error) {
            set_last_error(use_agent ? "Agent cleanup failed" : std::string(error.what()));
            kLog.warning("Transcript cleanup failed; using raw text ({})",
                         detail::exception_kind(error));
        } catch (...) {
            set_last_error(std::string("unknown error"));
            kLog.warning("Transcript cleanup failed; using raw text ({})", "Exception");
        }
        return std::string(text);
    }
    std::string cleaned = py::strip(pending->result);
    if (cleaned.empty()) {
        set_last_error("empty response");
        kLog.warning("Transcript cleanup returned empty; using raw text");
        return std::string(text);
    }
    set_last_error(std::nullopt);
    return cleaned;
}

std::pair<std::string, std::optional<std::string>>
polish_cleanup_rule(std::string_view instruction, std::string_view polish_prompt,
                    std::shared_ptr<http::Client> http, KeySources keys, SettingsReader settings,
                    std::optional<std::string_view> provider, std::optional<std::string_view> model,
                    std::optional<std::string_view> reasoning, const CancelToken& cancel,
                    CleanupOptions options) {
    const std::string stripped = py::strip(instruction);
    if (stripped.empty()) {
        return {"", std::string("empty instruction")};
    }
    TranscriptCleanup cleaner(std::move(http), keys, std::move(settings), provider.value_or(""),
                              model.value_or(""), reasoning.value_or(""), false,
                              std::move(options));
    const std::string result =
        cleaner.cleanup(stripped, polish_prompt, std::nullopt, std::nullopt, cancel);
    return {py::strip(result), cleaner.last_error()};
}

} // namespace fv::llm
