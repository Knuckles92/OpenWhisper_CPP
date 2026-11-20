#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/env.h"
#include "base/json.h"
#include "base/log.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "http/client.h"
#include "llm/credentials.h"
#include "llm/openai_client.h"
#include "llm/profiles.h"
#include "pyport/errors.h"

// services/typesafe.py's routes, answer types and settings helpers (69-184, 615-700): Fast
// judgments ask narrow typed questions ("noul" yes/no probabilities, "choice" with a confidence,
// "score") of TypeSafe's Jev model, directly or through OpenRouter, or of one of the user's own
// text models (llm/text_judge.h). Every failure is "no judgment": no feature built on a judge
// ever sees an exception. Every caller is gated by typesafe_enabled and, inside a meeting, by
// that meeting's AI insights consent unless the judge runs on this computer
// (JudgeRoute::is_local).
namespace fv::llm {

inline constexpr std::string_view kTypeSafeEndpoint = "https://api.typesafe.ai/v1/systemone";
// Pinned rather than jev-latest: the features' thresholds were measured on it.
inline constexpr std::string_view kTypeSafeModel = "jev-1.13.0";
// OpenRouter's id for the same 1.13 release (it has no patch digit).
inline constexpr std::string_view kOpenRouterJudgeModel = "typesafe/jev-1.13";
inline constexpr std::string_view kTypeSafeCredentialEnv = "TYPESAFE_API_KEY";
// Seconds: the p95 plus a slow network, short enough that a stalled judgment cannot hold a
// worker thread for a noticeable stretch.
inline constexpr double kJudgeDefaultTimeoutS = 4.0;
// Seconds: a text model writes its answer token by token; kept under a minute so live
// highlights, asked once per completed minute, never fall behind the meeting.
inline constexpr double kTextModelJudgeTimeoutS = 30.0;
// Bound on the JSON request (code points), under the documented 32k-token state budget.
inline constexpr std::size_t kMaxStateChars = 24'000;
// Seconds between two warnings of one judge; the others go to DEBUG.
inline constexpr double kJudgeWarnSpacingS = 60.0;
// Where a profile without a base URL (OpenAI's) sends requests.
inline constexpr std::string_view kJudgeOpenAiBaseUrl = "https://api.openai.com/v1";

// TypeSafeProvider: the values of typesafe_provider.
inline constexpr std::string_view kJudgeProviderTypeSafe = "typesafe";
inline constexpr std::string_view kJudgeProviderOpenRouter = "openrouter";
inline constexpr std::string_view kJudgeProviderTextModel = "text_model";
// TypeSafeTextModel: the values of typesafe_text_model.
inline constexpr std::string_view kJudgeTextModelMeeting = "meeting";
inline constexpr std::string_view kJudgeTextModelCleanup = "cleanup";

// One way to reach a judge: endpoint, key and model id there.
struct JudgeRoute {
    std::string id;
    std::string name;
    std::string endpoint;
    std::string model;
    // Empty when the endpoint takes no key (a local server).
    std::string credential_env;
    // Sent with every request on top of the auth and content headers.
    http::Headers headers;
    // Budget for one judgment (seconds) unless the caller allows less.
    double timeout_s = kJudgeDefaultTimeoutS;
    // The text model profile that answers on the text-model route; nullopt for Jev.
    std::optional<TextLLMProfile> profile;

    // Judgments are answered on this computer and never leave it.
    bool is_local() const;
    // urlsplit(endpoint).hostname, else the endpoint itself.
    std::string host() const;
    // "<name> API key".
    std::string key_label() const;
    // key_label with its article ("an" before a vowel), for "Add ..." copy.
    std::string key_phrase() const;

    bool operator==(const JudgeRoute&) const = default;
};

// TYPESAFE_ROUTE and OPENROUTER_ROUTE (built from the OpenRouter text profile, so its base URL,
// key variable and attribution header cannot drift from cleanup's).
const JudgeRoute& typesafe_route();
const JudgeRoute& openrouter_route();
// ROUTES: the two routes to Jev itself, in that order.
const std::vector<JudgeRoute>& jev_routes();

// A malformed request or response; never carries the API key.
class TypeSafeError : public py::PyError {
public:
    explicit TypeSafeError(const std::string& message) : py::PyError("TypeSafeError", message) {}
};

// One Choice answer: the selected option and how concentrated the distribution is.
struct ChoiceAnswer {
    std::string choice;
    double confidence = 0.0;
    Json probabilities = Json::object(); // option -> probability; {} when not available
    bool operator==(const ChoiceAnswer&) const = default;
};

struct JudgeUsage {
    std::int64_t requests = 0;
    std::int64_t failures = 0;
    std::int64_t input_tokens = 0; // as the provider reports them
    double last_latency_s = 0.0;
};

// Anything callers can ask: a concrete judge or a FollowingJudge.
// Affinity: thread-safe.
class Judge {
public:
    virtual ~Judge() = default;
    // Validated answers keyed by question name, or nullopt (every failure is "no judgment").
    // `questions` is {name: {"type", "instructions", "criteria"?}}; `timeout_s` can only shorten
    // the judge's own budget. Blocks; never on the GUI thread. A cancelled token ends the wait
    // at once.
    virtual std::optional<Json> ask(const Json& state, const Json& questions,
                                    std::optional<double> timeout_s, const CancelToken& cancel) = 0;
    // The model that answers the next question; "" when none would.
    virtual std::string model() const = 0;
    virtual bool is_local() const = 0;

    // The probability that the yes/no `instructions` hold for `state`.
    std::optional<double> noul(const Json& state, const Json& instructions,
                               const Json& criteria = Json(),
                               std::optional<double> timeout_s = std::nullopt,
                               const CancelToken& cancel = {});
    // One option of `criteria` ({option: description}) for `state`.
    std::optional<ChoiceAnswer> choice(const Json& state, const Json& instructions,
                                       const Json& criteria, const CancelToken& cancel = {});
};

namespace detail {
class KeepAlivePool;
} // namespace detail

// What tests replace in a JudgeEnvironment; the defaults are the real ones.
struct JudgeHooks {
    // A new keep-alive connection: an http::Client with a connection cache of its own.
    std::function<std::shared_ptr<http::Client>()> new_connection;
    // The proxy urllib would use for a URL, nullopt to connect directly; may throw.
    std::function<std::optional<std::string>(std::string_view url)> proxy_for;
    // create_openai_client for the text-model route (no retries: a failed judgment is just no
    // judgment).
    std::function<OpenAiCompatClient(const TextLLMProfile&, double timeout_s,
                                     std::string_view api_key)>
        make_text_client;
    // The keep-alive pool's clock for its idle window, and its limits.
    const Clock* pool_clock = nullptr;
    std::size_t pool_max_idle = 4;
    double pool_idle_s = 115.0;
    // Starts run_cancellable's worker thread (std::thread when unset). Tests throw
    // std::system_error from it to take the path where no thread can be started.
    std::function<std::thread(std::function<void()>)> start_thread;
};

// Everything judges share in a process: the HTTP client, where keys are found, the keep-alive
// pool of the Jev routes, the text-model route's client cache and the endpoints that refused a
// JSON schema, and the requests a cancel left running. Build one per process; it must outlive
// every judge built over it, and its destructor cancels and joins what is still running.
// Affinity: thread-safe.
class JudgeEnvironment {
public:
    JudgeEnvironment(std::shared_ptr<http::Client> http, CredentialStore& store,
                     const EnvReader& env, const DotEnv& dotenv, JudgeHooks hooks = {});
    ~JudgeEnvironment();
    JudgeEnvironment(const JudgeEnvironment&) = delete;
    JudgeEnvironment& operator=(const JudgeEnvironment&) = delete;

    const std::shared_ptr<http::Client>& http() const;
    KeySources& keys();
    detail::KeepAlivePool& pool();
    // The proxy lookup _proxied consults.
    const std::function<std::optional<std::string>(std::string_view)>& proxy_lookup() const;

    // _client: one OpenAI-compatible client per (profile id, kind, base URL, key variable, key),
    // at most four, the oldest dropped first.
    OpenAiCompatClient text_client(const TextLLMProfile& profile, double timeout_s,
                                   std::string_view api_key);
    // _NO_SCHEMA: endpoints and models that rejected a JSON schema get plain JSON instructions.
    bool schema_refused(std::string_view base_url, std::string_view model) const;
    void refuse_schema(std::string_view base_url, std::string_view model);

    // Runs `work` on a thread of its own and waits for it to finish, or for `cancel`. False when
    // the cancel came first: the work's own token is then cancelled and the work is left to end
    // on its thread, joined later. When no thread can be started, the work runs on the caller's.
    // An optional deadline also bounds scheduling and dispatch time. Without a worker,
    // deadline-bound work fails promptly instead of running inline.
    bool run_cancellable(std::function<void(const CancelToken&)> work, const CancelToken& cancel,
                         std::optional<std::chrono::steady_clock::time_point> deadline = {});

private:
    struct State;
    std::unique_ptr<State> state_;
};

// The judge options Python's **kwargs carry.
struct JudgeOptions {
    std::optional<std::string> model; // the route's when unset
    std::optional<double> timeout_s;  // the route's when unset
    const Clock* clock = nullptr;     // usage latency and warning spacing; system_clock()
};

// _RouteJudge: what every concrete judge shares: its route, budget, usage and sparse warnings.
// Affinity: thread-safe.
class RouteJudge : public Judge {
public:
    const JudgeRoute& route() const { return route_; }
    std::string model() const override { return model_; }
    bool is_local() const override { return route_.is_local(); }
    double timeout_s() const { return timeout_s_; }
    JudgeUsage usage() const;
    // True when `other` would send to the same place with the same key.
    virtual bool same_connection(const Judge& other) const = 0;

protected:
    RouteJudge(JudgeRoute route, const JudgeOptions& options, JudgeEnvironment& environment,
               const log::Logger& logger);

    // The caller's budget only shortens the judge's (seconds).
    double budget(std::optional<double> timeout_s) const;
    // Seconds on the judge's clock.
    double now() const;
    void record(double started, bool ok, std::int64_t tokens = 0);
    // A warning at most once per kJudgeWarnSpacingS, else DEBUG. Never the state, the questions
    // or the key.
    void warn(const std::string& message);
    void debug(const std::string& message) const;
    JudgeEnvironment& environment() const { return environment_; }

private:
    JudgeRoute route_;
    std::string model_;
    double timeout_s_;
    JudgeEnvironment& environment_;
    const Clock& clock_;
    const log::Logger& logger_;
    mutable std::mutex mutex_;
    double last_warn_;
    JudgeUsage usage_;
};

// The text-model route over the model Settings > Fast judgments names: the AI insights model
// unless AI cleanup's was chosen (typesafe_text_model "cleanup"); an unknown profile is
// OpenRouter's.
JudgeRoute text_model_route(const Json& settings);
// The route Settings > Fast judgments names, TypeSafe unless another was chosen.
JudgeRoute route_from_settings(const Json& settings);
// The chosen route answers on this computer, so no consent to share is needed.
bool route_is_local(const Json& settings);
// The key `route` sends: "dummy" for an endpoint without a key variable, else the resolved
// credential; nullopt when it cannot be resolved. Reads the credential store.
std::optional<std::string> route_key(const JudgeRoute& route, KeySources& keys);
// The chosen route's key resolves from the store, the environment or .env (separate from
// is_configured, so the UI can tell "switched off" from "on but unusable").
bool key_present(const Json& settings, CredentialStore& store, const EnvReader& env,
                 const DotEnv& dotenv);
// The same for one route, whatever Settings say.
bool key_present(const JudgeRoute& route, KeySources& keys);
// typesafe_enabled is on and the chosen route's key resolves.
bool is_configured(const Json& settings, CredentialStore& store, const EnvReader& env,
                   const DotEnv& dotenv);
// A judge on the chosen route when Fast judgments are on and its key resolves, else null: a
// TypeSafeJudge for Jev, a TextModelJudge for the text-model route.
std::shared_ptr<Judge> judge_from_settings(const Json& settings, JudgeEnvironment& environment,
                                           const JudgeOptions& options = {});

// FollowingJudge: asks through whichever judge `provider` returns at the moment of asking, so a
// worker that lives as long as a meeting follows a switch of Fast judgments to another route.
// Affinity: thread-safe when the provider is.
class FollowingJudge final : public Judge {
public:
    explicit FollowingJudge(std::function<std::shared_ptr<Judge>()> provider);
    std::optional<Json> ask(const Json& state, const Json& questions,
                            std::optional<double> timeout_s, const CancelToken& cancel) override;
    // "" when no judge would answer.
    std::string model() const override;
    // False when no judge would answer.
    bool is_local() const override;

private:
    std::function<std::shared_ptr<Judge>()> provider_;
};

// What Route by voice and the Fast judgments page ask; the app's implementation reads the
// settings snapshot it is given and the credential store, so it blocks and never runs on the
// GUI thread.
class JudgeProvider {
public:
    virtual ~JudgeProvider() = default;
    virtual std::shared_ptr<Judge> judge(const Json& settings) = 0; // judge_from_settings
    virtual JudgeRoute route(const Json& settings) = 0;             // route_from_settings
    virtual bool key_present(const Json& settings) = 0;
};

// Over `environment`, which must outlive it.
std::unique_ptr<JudgeProvider> make_judge_provider(JudgeEnvironment& environment);

} // namespace fv::llm
