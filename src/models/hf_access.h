#pragma once

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/clock.h"
#include "base/env.h"
#include "models/speech_model_cache.h"
#include "settings/store.h"

// Cache-first, consent-driven model downloads (services/hf_access.py): a cached model always
// loads with no network activity; the download server is contacted only when a requested model
// is missing AND the persisted policy (or a one-time grant) permits it. Named after Hugging Face
// because the pinned model files are on huggingface.co. OpenWhisper's Hugging Face hub cache walk,
// snapshot_download and Whisper repositories are not ported: the only models are the speech
// models of SpeechModelCache. CachedModelInfo is speech_model_cache.h's.
namespace fv::models {

// Outcome of evaluating a model request against cache and policy.
enum class AccessDecision { LoadCached, DownloadAllowed, NeedsConsent, BlockedByEnv };
// "load_cached", "download_allowed", "needs_consent", "blocked_by_env".
std::string_view access_decision_name(AccessDecision);

// The choices of the download consent dialog.
enum class ConsentAction { Cancel, DownloadOnce, AlwaysAllow, OpenSettings };
// "cancel", "download_once", "always_allow", "open_settings".
std::string_view consent_action_name(ConsentAction);

// The bundled approximate download size ("~714 MB"), or nullopt for an unknown model.
std::optional<std::string> format_download_size(std::string_view model_name);

// One link of a failure's cause chain, as describe_model_download_error reads it.
struct ErrorCause {
    std::string text;               // str(error)
    std::optional<int> errno_value; // an OSError's errno
    std::optional<int> winerror;    // an OSError's winerror
    // socket.gaierror, ConnectionError or TimeoutError
    bool network = false;
};

// The failure and its causes (std::nested_exception), at most 8 deep. std::system_error gives
// its errno (and on Windows its system error code as winerror, with ENOSPC for a full disk);
// a fv::download::DownloadError adds its network failure's reason as a cause; a cancelled
// download (ComponentCanceled, DownloadCanceled) reads "Model download canceled", the text
// OpenWhisper's download worker relays.
std::vector<ErrorCause> error_chain(const std::exception_ptr& error);

// Status-line copy for a failed model download, with what to do next; the raw error belongs in
// the log, which the caller writes. Checked in order: canceled, then disk full (naming the
// bundled size), then offline, else a pointer to identity().log_file. Every text ends "in
// Downloads." or "Retry in Downloads." so the status line links there.
std::string describe_model_download_error(std::string_view model_name,
                                          std::span<const ErrorCause> chain);
std::string describe_model_download_error(std::string_view model_name,
                                          const std::exception_ptr& error);

// The shared inventory of downloaded models, keyed by repository id (peek_cached_models,
// invalidate_cached_models_snapshot, scan_cached_models). Concurrent scans share one walk, and a
// scan that raced a download or delete (invalidate()) is discarded and taken again.
// Affinity: thread-safe; scan() blocks on the disk (never on the GUI thread), peek() does not.
class CachedModelsSnapshot {
public:
    using Inventory = std::map<std::string, CachedModelInfo, std::less<>>;
    // `source` lists the downloaded models (SpeechModelCache::inventory).
    using Source = std::function<std::vector<CachedModelInfo>()>;

    explicit CachedModelsSnapshot(const SpeechModelCache& cache,
                                  const Clock& clock = system_clock());
    explicit CachedModelsSnapshot(Source source, const Clock& clock = system_clock());

    // The last completed scan without touching the disk; nullopt before the first one.
    std::optional<Inventory> peek() const;
    // Discards the inventory after a successful download or delete.
    void invalidate();
    // A fresh inventory, or with a positive `max_age_seconds` the last one when it is at most
    // that old. A scan already running is waited for and its result shared. Exceptions of the
    // source propagate; the next scan starts afresh.
    Inventory scan(double max_age_seconds = 0.0);

private:
    Source source_;
    const Clock& clock_;
    mutable std::mutex mutex_;
    std::condition_variable done_;
    bool in_progress_ = false;
    std::uint64_t epoch_ = 0;
    std::optional<Inventory> snapshot_;
    std::chrono::steady_clock::time_point snapshot_at_{};
};

// Cache detection, policy evaluation, one-time grants and request deduplication.
// Affinity: thread-safe: model loads and consent requests come from worker threads while
// dialogs run on the GUI thread.
class HuggingFaceAccessCoordinator {
public:
    // Whether a model is fully downloaded (SpeechModelCache::is_cached for a catalog model).
    using IsCached = std::function<bool(std::string_view model_name)>;

    HuggingFaceAccessCoordinator(SettingsStore& settings, IsCached is_cached,
                                 const EnvReader& env = system_env());

    // The persisted policy (load_hf_access_policy, with its legacy migration).
    std::string policy() const;
    // Persists a new policy (save_hf_access_policy); std::invalid_argument for an unknown one.
    void set_policy(std::string_view policy);

    // Authorizes one download of `model_name` without changing the policy; consumed by the next
    // evaluate_access that is not advisory.
    void grant_once(std::string_view model_name);
    // Cached: LoadCached. HF_HUB_OFFLINE set: BlockedByEnv, even against a grant. A grant:
    // DownloadAllowed (consumed unless `advisory`). Policy "always": DownloadAllowed. Otherwise
    // NeedsConsent ("never" too: its dialog offers the one-time override).
    AccessDecision evaluate_access(std::string_view model_name, bool advisory = false);

    // Claims the consent and download slot of a model: false while one is held, so only one
    // dialog and one download exist per model.
    bool begin_request(std::string_view model_name);
    // Releases a slot; harmless for an unclaimed model.
    void end_request(std::string_view model_name);
    // Claims a slot for every model a batch may fetch, skipping cached models, duplicates and
    // models already in flight. The caller releases each returned name with end_request. Grants
    // are not issued here: the download worker grants each model just before fetching it, so a
    // queue stopped early leaves no unconsumed consent behind.
    std::vector<std::string> claim_batch(std::span<const std::string> model_names);
    int requests_in_flight() const;

private:
    SettingsStore& settings_;
    IsCached is_cached_;
    const EnvReader& env_;
    mutable std::mutex mutex_;
    std::set<std::string, std::less<>> grants_;
    std::set<std::string, std::less<>> active_;
};

} // namespace fv::models
