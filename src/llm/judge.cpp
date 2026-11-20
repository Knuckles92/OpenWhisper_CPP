#include "llm/judge.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <set>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

#include "http/proxy.h"
#include "llm/cleanup_settings.h"
#include "llm/detail/keep_alive.h"
#include "llm/detail/truthy.h"
#include "llm/detail/url.h"
#include "llm/text_judge.h"
#include "llm/typesafe.h"
#include "pyport/text.h"
#include "settings/keys.h"

namespace fv::llm {

namespace {

constexpr log::Logger kLog("services.typesafe");

JudgeRoute make_openrouter_route() {
    const auto profile = builtin_profile(kOpenRouterProfileId);
    JudgeRoute route;
    route.id = std::string(kJudgeProviderOpenRouter);
    route.name = "OpenRouter";
    route.endpoint = py::rstrip(profile->base_url.value_or(""), "/") + "/systemone";
    route.model = std::string(kOpenRouterJudgeModel);
    route.credential_env = profile->api_key_env;
    route.headers = provider_headers(*profile).value_or(http::Headers{});
    return route;
}

// One request on a thread of its own. The waiter and the thread share it; whoever finishes
// last frees it.
struct Pending {
    explicit Pending(const CancelToken& parent) : source(parent) {}
    CancelSource source;
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    std::chrono::steady_clock::time_point finished_at;
    std::thread thread;
};

using ClientKey =
    std::tuple<std::string, std::string, std::optional<std::string>, std::string, std::string>;

} // namespace

bool JudgeRoute::is_local() const {
    return profile.has_value() && profile->is_local();
}

std::string JudgeRoute::host() const {
    try {
        if (auto host = detail::urlsplit(endpoint).hostname()) {
            return *host;
        }
    } catch (const std::exception&) {
        // urlsplit refuses the URL; the endpoint itself names the place.
    }
    return endpoint;
}

std::string JudgeRoute::key_label() const {
    return name + " API key";
}

std::string JudgeRoute::key_phrase() const {
    const std::string first = py::lower(py::slice(name, 0, 1));
    const bool vowel = first.empty() || first == "a" || first == "e" || first == "i" ||
                       first == "o" || first == "u";
    return std::string(vowel ? "an" : "a") + " " + key_label();
}

const JudgeRoute& typesafe_route() {
    static const JudgeRoute route = [] {
        JudgeRoute built;
        built.id = std::string(kJudgeProviderTypeSafe);
        built.name = "TypeSafe";
        built.endpoint = std::string(kTypeSafeEndpoint);
        built.model = std::string(kTypeSafeModel);
        built.credential_env = std::string(kTypeSafeCredentialEnv);
        return built;
    }();
    return route;
}

const JudgeRoute& openrouter_route() {
    static const JudgeRoute route = make_openrouter_route();
    return route;
}

const std::vector<JudgeRoute>& jev_routes() {
    static const std::vector<JudgeRoute> routes{typesafe_route(), openrouter_route()};
    return routes;
}

std::optional<double> Judge::noul(const Json& state, const Json& instructions, const Json& criteria,
                                  std::optional<double> timeout_s, const CancelToken& cancel) {
    Json question = Json::object();
    question["type"] = "noul";
    question["instructions"] = instructions;
    if (detail::truthy(criteria)) {
        question["criteria"] = criteria;
    }
    Json questions = Json::object();
    questions["q"] = std::move(question);
    const auto answers = ask(state, questions, timeout_s, cancel);
    if (!answers || !answers->contains("q")) {
        return std::nullopt;
    }
    return answers->at("q").at("noul").get<double>();
}

std::optional<ChoiceAnswer> Judge::choice(const Json& state, const Json& instructions,
                                          const Json& criteria, const CancelToken& cancel) {
    Json question = Json::object();
    question["type"] = "choice";
    question["instructions"] = instructions;
    question["criteria"] = criteria;
    Json questions = Json::object();
    questions["q"] = std::move(question);
    const auto answers = ask(state, questions, std::nullopt, cancel);
    if (!answers || !answers->contains("q")) {
        return std::nullopt;
    }
    const Json& answer = answers->at("q");
    return ChoiceAnswer{answer.at("choice").get<std::string>(),
                        answer.at("confidence").get<double>(),
                        answer.value("probabilities", Json::object())};
}

// ---------------------------------------------------------------------------------------------
// JudgeEnvironment

struct JudgeEnvironment::State {
    State(std::shared_ptr<http::Client> client, CredentialStore& store, const EnvReader& env,
          const DotEnv& dotenv, JudgeHooks given)
        : http(std::move(client)), keys{store, env, dotenv}, hooks(std::move(given)) {
        if (!hooks.new_connection) {
            hooks.new_connection = [] {
                // Only an unproxied endpoint is pooled, so the connection never asks for one.
                return http::make_curl_client(http::CurlClientOptions{[](std::string_view) {
                    return std::optional<std::string>();
                }});
            };
        }
        if (!hooks.proxy_for) {
            hooks.proxy_for = [](std::string_view url) {
                return http::proxy_for_url(url, http::read_system_proxy_settings());
            };
        }
        if (!hooks.make_text_client) {
            hooks.make_text_client = [client = http](const TextLLMProfile& profile,
                                                     double timeout_s, std::string_view key) {
                return create_openai_client(client, profile, timeout_s, key, {}, 0);
            };
        }
        if (!hooks.start_thread) {
            hooks.start_thread = [](std::function<void()> body) {
                return std::thread(std::move(body));
            };
        }
        pool = std::make_unique<detail::KeepAlivePool>(hooks.new_connection, hooks.pool_max_idle,
                                                       hooks.pool_idle_s, hooks.pool_clock);
    }

    // Joins the abandoned requests that have ended, or with `all` every one after cancelling it.
    void reap(bool all) {
        std::vector<std::shared_ptr<Pending>> joining;
        {
            std::lock_guard lock(mutex);
            auto split = std::partition(abandoned.begin(), abandoned.end(),
                                        [all](const std::shared_ptr<Pending>& pending) {
                                            if (all) {
                                                return false;
                                            }
                                            std::lock_guard done_lock(pending->mutex);
                                            return !pending->done;
                                        });
            joining.assign(split, abandoned.end());
            abandoned.erase(split, abandoned.end());
        }
        for (const auto& pending : joining) {
            pending->source.cancel();
            if (pending->thread.joinable()) {
                pending->thread.join();
            }
        }
    }

    std::shared_ptr<http::Client> http;
    KeySources keys;
    JudgeHooks hooks;
    std::unique_ptr<detail::KeepAlivePool> pool;

    mutable std::mutex mutex; // clients, no_schema, abandoned
    std::vector<std::pair<ClientKey, OpenAiCompatClient>> clients;
    std::set<std::pair<std::string, std::string>, std::less<>> no_schema;
    std::vector<std::shared_ptr<Pending>> abandoned;
};

JudgeEnvironment::JudgeEnvironment(std::shared_ptr<http::Client> http, CredentialStore& store,
                                   const EnvReader& env, const DotEnv& dotenv, JudgeHooks hooks)
    : state_(std::make_unique<State>(std::move(http), store, env, dotenv, std::move(hooks))) {}

JudgeEnvironment::~JudgeEnvironment() {
    state_->reap(true);
}

const std::shared_ptr<http::Client>& JudgeEnvironment::http() const {
    return state_->http;
}

KeySources& JudgeEnvironment::keys() {
    return state_->keys;
}

detail::KeepAlivePool& JudgeEnvironment::pool() {
    return *state_->pool;
}

const std::function<std::optional<std::string>(std::string_view)>&
JudgeEnvironment::proxy_lookup() const {
    return state_->hooks.proxy_for;
}

OpenAiCompatClient JudgeEnvironment::text_client(const TextLLMProfile& profile, double timeout_s,
                                                 std::string_view api_key) {
    ClientKey key{profile.id, profile.kind, profile.base_url, profile.api_key_env,
                  std::string(api_key)};
    std::lock_guard lock(state_->mutex);
    for (const auto& [existing, client] : state_->clients) {
        if (existing == key) {
            return client;
        }
    }
    if (state_->clients.size() >= 4) {
        state_->clients.erase(state_->clients.begin());
    }
    auto client = state_->hooks.make_text_client(profile, timeout_s, api_key);
    state_->clients.emplace_back(std::move(key), client);
    return client;
}

bool JudgeEnvironment::schema_refused(std::string_view base_url, std::string_view model) const {
    std::lock_guard lock(state_->mutex);
    return state_->no_schema.contains(std::pair<std::string, std::string>(base_url, model));
}

void JudgeEnvironment::refuse_schema(std::string_view base_url, std::string_view model) {
    std::lock_guard lock(state_->mutex);
    state_->no_schema.emplace(base_url, model);
}

bool JudgeEnvironment::run_cancellable(
    std::function<void(const CancelToken&)> work, const CancelToken& cancel,
    std::optional<std::chrono::steady_clock::time_point> deadline) {
    state_->reap(false);
    if (cancel.cancelled() || (deadline && std::chrono::steady_clock::now() >= *deadline)) {
        return false;
    }
    auto pending = std::make_shared<Pending>(cancel);
    const CancelToken token = pending->source.token();
    auto shared_work = std::make_shared<std::function<void(const CancelToken&)>>(std::move(work));
    try {
        pending->thread = state_->hooks.start_thread([pending, token, shared_work] {
            try {
                (*shared_work)(token);
            } catch (...) {
                // The work keeps its own outcome; nothing may escape the thread.
            }
            {
                std::lock_guard lock(pending->mutex);
                pending->finished_at = std::chrono::steady_clock::now();
                pending->done = true;
            }
            pending->changed.notify_all();
        });
    } catch (const std::system_error&) {
        if (deadline) {
            return false;
        }
        // No thread to spare: the work runs here, where a cancel still ends its request, only
        // later than the thread would let it; ask never throws.
        try {
            (*shared_work)(token);
        } catch (...) {
            // As on the thread.
        }
        return !cancel.cancelled();
    }
    const Subscription wake = cancel.on_cancel([pending] {
        { std::lock_guard lock(pending->mutex); }
        pending->changed.notify_all();
    });
    bool finished = false;
    {
        std::unique_lock lock(pending->mutex);
        const auto settled = [&] {
            return pending->done || cancel.cancelled();
        };
        if (deadline) {
            pending->changed.wait_until(lock, *deadline, settled);
        } else {
            pending->changed.wait(lock, settled);
        }
        finished = pending->done && (!deadline || pending->finished_at <= *deadline);
    }
    if (finished && !deadline) {
        pending->thread.join();
        return true;
    }
    if (!finished) {
        pending->source.cancel();
    }
    // Even after publishing its result, a deadline-bound worker may be descheduled before
    // it exits. The environment owns that worker until it can be joined.
    std::lock_guard lock(state_->mutex);
    state_->abandoned.push_back(std::move(pending));
    return finished;
}

// ---------------------------------------------------------------------------------------------
// RouteJudge

RouteJudge::RouteJudge(JudgeRoute route, const JudgeOptions& options, JudgeEnvironment& environment,
                       const log::Logger& logger)
    : route_(std::move(route)), environment_(environment),
      clock_(options.clock != nullptr ? *options.clock : system_clock()), logger_(logger),
      last_warn_(-kJudgeWarnSpacingS) {
    model_ = options.model && !options.model->empty() ? *options.model : route_.model;
    timeout_s_ = options.timeout_s.value_or(route_.timeout_s);
}

JudgeUsage RouteJudge::usage() const {
    std::lock_guard lock(mutex_);
    return usage_;
}

double RouteJudge::budget(std::optional<double> timeout_s) const {
    return timeout_s ? std::min(*timeout_s, timeout_s_) : timeout_s_;
}

double RouteJudge::now() const {
    return std::chrono::duration<double>(clock_.steady().time_since_epoch()).count();
}

void RouteJudge::record(double started, bool ok, std::int64_t tokens) {
    const double latency = now() - started;
    std::lock_guard lock(mutex_);
    ++usage_.requests;
    if (!ok) {
        ++usage_.failures;
    }
    usage_.input_tokens += tokens;
    usage_.last_latency_s = latency;
}

void RouteJudge::warn(const std::string& message) {
    const double at = now();
    {
        std::lock_guard lock(mutex_);
        if (at - last_warn_ < kJudgeWarnSpacingS) {
            logger_.debug("{}", message);
            return;
        }
        last_warn_ = at;
    }
    logger_.warning("{}", message);
}

void RouteJudge::debug(const std::string& message) const {
    logger_.debug("{}", message);
}

// ---------------------------------------------------------------------------------------------
// Settings helpers

JudgeRoute text_model_route(const Json& settings) {
    std::optional<TextLLMProfile> profile;
    std::string model;
    if (keys::typesafe_text_model.read(settings) == kJudgeTextModelCleanup) {
        profile = get_profile(resolve_transcript_cleanup_provider(settings), settings);
        model = resolve_transcript_cleanup_model(settings);
    } else {
        profile = resolve_meeting_llm_profile(settings);
        model = resolve_meeting_llm_model(settings);
    }
    if (!profile) {
        // Both resolvers fall back to OpenRouter for an unknown profile id.
        profile = builtin_profile(kOpenRouterProfileId);
    }
    JudgeRoute route;
    route.id = std::string(kJudgeProviderTextModel);
    route.name = profile->name;
    route.endpoint = profile->base_url && !profile->base_url->empty()
                         ? *profile->base_url
                         : std::string(kJudgeOpenAiBaseUrl);
    route.model = std::move(model);
    route.credential_env = profile->api_key_env;
    route.headers = provider_headers(*profile).value_or(http::Headers{});
    route.timeout_s = kTextModelJudgeTimeoutS;
    route.profile = std::move(profile);
    return route;
}

JudgeRoute route_from_settings(const Json& settings) {
    const std::string provider = keys::typesafe_provider.read(settings);
    if (provider == kJudgeProviderTextModel) {
        return text_model_route(settings);
    }
    for (const auto& route : jev_routes()) {
        if (route.id == provider) {
            return route;
        }
    }
    return typesafe_route();
}

bool route_is_local(const Json& settings) {
    return route_from_settings(settings).is_local();
}

std::optional<std::string> route_key(const JudgeRoute& route, KeySources& keys) {
    if (route.credential_env.empty()) {
        // The OpenAI client insists on some key even for a server without auth.
        return std::string(kAuthFreeApiKey);
    }
    return lookup_env_value(route.credential_env, keys);
}

bool key_present(const JudgeRoute& route, KeySources& keys) {
    const auto key = route_key(route, keys);
    return key.has_value() && !key->empty();
}

bool key_present(const Json& settings, CredentialStore& store, const EnvReader& env,
                 const DotEnv& dotenv) {
    KeySources keys{store, env, dotenv};
    return key_present(route_from_settings(settings), keys);
}

bool is_configured(const Json& settings, CredentialStore& store, const EnvReader& env,
                   const DotEnv& dotenv) {
    return keys::typesafe_enabled.read(settings) && key_present(settings, store, env, dotenv);
}

std::shared_ptr<Judge> judge_from_settings(const Json& settings, JudgeEnvironment& environment,
                                           const JudgeOptions& options) {
    if (!keys::typesafe_enabled.read(settings)) {
        return nullptr;
    }
    JudgeRoute route = route_from_settings(settings);
    const auto key = route_key(route, environment.keys());
    if (!key || key->empty()) {
        kLog.debug("Fast judgments enabled but {} is not set", route.credential_env);
        return nullptr;
    }
    if (!route.profile) {
        TypeSafeJudgeOptions jev;
        jev.route = std::move(route);
        jev.model = options.model;
        jev.timeout_s = options.timeout_s;
        jev.clock = options.clock;
        return std::make_shared<TypeSafeJudge>(*key, environment, std::move(jev));
    }
    return std::make_shared<TextModelJudge>(*key, std::move(route), environment, options);
}

// ---------------------------------------------------------------------------------------------
// FollowingJudge and the provider

FollowingJudge::FollowingJudge(std::function<std::shared_ptr<Judge>()> provider)
    : provider_(std::move(provider)) {}

std::optional<Json> FollowingJudge::ask(const Json& state, const Json& questions,
                                        std::optional<double> timeout_s,
                                        const CancelToken& cancel) {
    const auto judge = provider_();
    if (!judge) {
        return std::nullopt;
    }
    return judge->ask(state, questions, timeout_s, cancel);
}

std::string FollowingJudge::model() const {
    const auto judge = provider_();
    return judge ? judge->model() : std::string();
}

bool FollowingJudge::is_local() const {
    const auto judge = provider_();
    return judge && judge->is_local();
}

namespace {

class EnvironmentJudgeProvider final : public JudgeProvider {
public:
    explicit EnvironmentJudgeProvider(JudgeEnvironment& environment) : environment_(environment) {}

    std::shared_ptr<Judge> judge(const Json& settings) override {
        return judge_from_settings(settings, environment_);
    }
    JudgeRoute route(const Json& settings) override { return route_from_settings(settings); }
    bool key_present(const Json& settings) override {
        return llm::key_present(route_from_settings(settings), environment_.keys());
    }

private:
    JudgeEnvironment& environment_;
};

} // namespace

std::unique_ptr<JudgeProvider> make_judge_provider(JudgeEnvironment& environment) {
    return std::make_unique<EnvironmentJudgeProvider>(environment);
}

} // namespace fv::llm
