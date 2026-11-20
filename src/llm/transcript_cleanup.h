#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/json.h"
#include "http/client.h"
#include "llm/credentials.h"
#include "llm/openai_client.h"
#include "llm/profiles.h"

// services/transcript_cleanup.py: AI cleanup of a transcript through the text-model endpoint the
// user chose, with a wall-clock limit and a cancel that end the wait at once, and the learned-rule
// polish built on it. The dedicated transcript client can also inject an installed-agent runner.
namespace fv::llm {

// last_error when the caller's cancel ended the wait.
inline constexpr std::string_view kCanceledReason = "canceled";
// config.TRANSCRIPT_CLEANUP_MAX_RETRIES: the paste waits on cleanup, so a dictation retries once.
inline constexpr int kTranscriptCleanupMaxRetries = 1;
// config.TRANSCRIPT_BATCH_CLEANUP_MAX_RETRIES: nothing waits to paste a batch.
inline constexpr int kTranscriptBatchCleanupMaxRetries = 2;
// A server the user runs may load the model before generating (seconds per attempt).
inline constexpr double kSelfHostedCleanupTimeoutS = 120.0;
// CLI startup and sign-in of an installed agent take longer than an HTTP request (seconds).
inline constexpr double kAgentCleanupTimeoutS = 60.0;
// Slack on top of the attempts' timeouts for the retry backoff (~0.5 s before the one retry).
inline constexpr double kRetryBackoffAllowanceS = 1.0;
// How often a waiting cleanup looks at the cancel token.
inline constexpr std::chrono::milliseconds kCancelPoll{50};

// The provider and model that produced a cleaned transcript, as history records it.
struct CleanupInfo {
    std::string provider;
    std::string model;
    double elapsed_s = 0.0;
    // The preset that ran (light, medium, high), "custom", "profile", or "".
    std::string level;
    bool operator==(const CleanupInfo&) const = default;
};

// The settings as they are now (Python's services.text_llm._load_settings).
using SettingsReader = std::function<Json()>;

// The environment variable holding a provider's key: "" for an installed agent, the profile's
// variable, else OpenRouter's or (for any other id) OpenAI's.
std::string provider_env_key(std::string_view provider, const Json& settings);
// The key cleanup would send: nullopt for an installed agent or a missing key, "dummy" for an
// endpoint that needs none. Blocking (credential store).
std::optional<std::string> find_api_key(std::string_view provider, const Json& settings,
                                        KeySources& keys);
// The chat models of `provider`'s endpoint (list_chat_models). Throws CatalogError
// ("Unknown text model endpoint '<id>'" for an id that names no profile, else what
// list_chat_models throws).
std::vector<std::string> list_cleanup_models(http::Client& client, std::string_view provider,
                                             std::optional<std::string_view> sort,
                                             const Json& settings, KeySources& keys,
                                             std::string_view api_key = {},
                                             const CancelToken& cancel = {});

// _attempt_budget: the per-attempt timeout, the retries, and the wall-clock limit of every
// attempt together (attempt * (retries + 1) + 1 s, or `deadline_s`). A per-call `timeout_s`
// marks a long job nobody waits to paste: it gets the batch retries.
struct AttemptBudget {
    double attempt_timeout_s = 0.0;
    int max_retries = 0;
    double give_up_s = 0.0;
    bool overridden = false; // timeout_s was given: the call uses a with_options copy
};
AttemptBudget attempt_budget(double client_timeout_s, std::optional<double> timeout_s,
                             std::optional<double> deadline_s);

// What a rewrite needs of a cleaner: cleanup() hands its input back on failure, so only an unset
// last_error() counts as a result.
class CleanupClient {
public:
    virtual ~CleanupClient() = default;
    virtual std::string cleanup(std::string_view text, std::string_view system_prompt = {},
                                std::optional<double> timeout_s = std::nullopt,
                                std::optional<double> deadline_s = std::nullopt,
                                const CancelToken& cancel = {}) = 0;
    virtual std::optional<std::string> last_error() const = 0;
    // The client's own per-attempt timeout in seconds; 0 when it has none.
    virtual double attempt_timeout_s() const { return 0.0; }
};

struct CleanupAgentRequest {
    std::string provider;
    std::string model;
    std::string text;
    std::string system_prompt;
    std::string reasoning;
    double timeout_s;
};

// The rest of OpenWhisper's constructor arguments, and the seams tests use.
struct CleanupOptions {
    // Sent instead of the resolved credential (Python's api_key=).
    std::optional<std::string> api_key;
    // config.TRANSCRIPT_CLEANUP_PROMPT, sent when cleanup() gets an empty system prompt. The
    // caller passes text::legacy_default_prompt().
    std::string default_prompt;
    // The wall clock of the deadline; the system clock when null.
    const Clock* clock = nullptr;
    // The client's retry waits, jitter and clock.
    RetryHooks retry_hooks;
    // Builds the client in place of create_openai_client (profile, per-attempt timeout in
    // seconds, key), as OpenWhisper's tests replace it.
    std::function<OpenAiCompatClient(const TextLLMProfile&, double, const std::string&)>
        client_factory;
    // Both callbacks may run on a request thread. The runner uses no tools and observes cancel.
    std::function<bool(std::string_view provider)> agent_available;
    std::function<std::optional<std::string>(const CleanupAgentRequest&, const CancelToken&)>
        run_agent;
};

// Affinity: thread-safe. cleanup() blocks its caller (a job's worker thread) and never runs on
// the GUI thread; configure() reads keys and blocks too. The request runs on a thread of its
// own; a wait that gives up cancels it and returns, and the destructor joins what is left.
class TranscriptCleanup final : public CleanupClient {
public:
    // `defer_client` leaves the client, and the key lookup, to the first configure(). `keys` and
    // what `settings` reads must outlive the object.
    TranscriptCleanup(std::shared_ptr<http::Client> http, KeySources keys, SettingsReader settings,
                      std::string_view provider = {}, std::string_view model = {},
                      std::string_view reasoning = {}, bool defer_client = true,
                      CleanupOptions options = {});
    ~TranscriptCleanup() override;
    TranscriptCleanup(const TranscriptCleanup&) = delete;
    TranscriptCleanup& operator=(const TranscriptCleanup&) = delete;

    // Applies settings, rebuilding the client only when the provider or the endpoint's
    // connection (id, kind, URL, key variable, resolved key) changed. An unknown reasoning level
    // keeps the current one.
    void configure(std::string_view provider, std::string_view model,
                   std::optional<std::string_view> reasoning = std::nullopt);
    // The per-attempt timeout of the current client: 8 s, 120 s for a server the user runs, 60 s
    // for an installed agent.
    double attempt_timeout_s() const override;
    bool is_available() const;

    // The cleaned text, or `text` itself when it is blank, cleanup is unavailable or the request
    // fails, times out or is canceled; last_error() is unset afterwards only on success ("empty
    // input", "cleanup unavailable", "canceled", "timed out after <N> s", "empty response", or
    // the failure's text). Returns within the attempts' timeouts plus a second even when the
    // provider keeps the response open, and within one poll (50 ms) of a cancel; a cancel set
    // before the call sends nothing.
    std::string cleanup(std::string_view text, std::string_view system_prompt = {},
                        std::optional<double> timeout_s = std::nullopt,
                        std::optional<double> deadline_s = std::nullopt,
                        const CancelToken& cancel = {}) override;
    // "not run" before the first cleanup().
    std::optional<std::string> last_error() const override;

    std::string provider() const;
    std::string model() const;
    std::string reasoning() const;
    std::optional<std::string> api_key() const;
    // A new number for each client built, 0 while there is none.
    std::uint64_t client_generation() const;
    std::optional<ConnectionFingerprint> connection() const;

private:
    struct Pending;

    std::string normalize_provider(std::string_view provider, const Json& settings) const;
    // _initialize_client; mutex_ held.
    void initialize_client(const Json& settings);

    void set_last_error(std::optional<std::string> error);
    void reap(bool all);

    std::shared_ptr<http::Client> http_;
    KeySources keys_;
    SettingsReader settings_;
    CleanupOptions options_;

    mutable std::mutex mutex_;
    std::string provider_;
    std::string model_;
    std::string reasoning_;
    std::optional<std::string> api_key_;
    std::optional<OpenAiCompatClient> client_;
    std::optional<ConnectionFingerprint> connection_;
    double timeout_s_;
    std::optional<std::string> last_error_ = "not run";
    std::uint64_t generation_ = 0;
    std::uint64_t next_generation_ = 0;

    std::mutex pending_mutex_;
    std::vector<std::shared_ptr<Pending>> abandoned_;
};

// polish_cleanup_rule: one short rule from a spoken instruction, through a cleaner of its own
// that sends `polish_prompt` (text::polish_prompt()) as the system prompt. Returns the stripped
// reply and last_error; on any failure the stripped instruction and the reason ("empty
// instruction" for a blank one, without a request). Blocking.
std::pair<std::string, std::optional<std::string>>
polish_cleanup_rule(std::string_view instruction, std::string_view polish_prompt,
                    std::shared_ptr<http::Client> http, KeySources keys, SettingsReader settings,
                    std::optional<std::string_view> provider = std::nullopt,
                    std::optional<std::string_view> model = std::nullopt,
                    std::optional<std::string_view> reasoning = std::nullopt,
                    const CancelToken& cancel = {}, CleanupOptions options = {});

} // namespace fv::llm
