#include "models/hf_access.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <system_error>

#include "base/identity.h"
#include "base/log.h"
#include "download/verified_download.h"
#include "models/catalog.h"
#include "models/hf_access_policy.h"
#include "models/model_details.h"
#include "pyport/text.h"

namespace fv::models {

namespace {

constexpr log::Logger kLog("services.hf_access");

// How a missing network reads once it has crossed a process boundary as text ("[Errno -3]
// Temporary failure in name resolution", WinError 11001, httpx's ConnectError, urllib's reason).
constexpr std::array<std::string_view, 12> kOfflineMarkers{
    "name resolution",
    "getaddrinfo",
    "name or service not known",
    "nodename nor servname",
    "network is unreachable",
    "no route to host",
    "connection refused",
    "connecterror",
    "failed to establish a new connection",
    "max retries exceeded",
    "could not reach the download server",
    "11001",
};
constexpr std::array<std::string_view, 4> kDiskFullMarkers{
    "no space left on device",
    "[errno 28]",
    "not enough space on the disk",
    "[winerror 112]",
};

// errno.ENOSPC as Python's errno module has it on every platform OpenWhisper ran on.
constexpr int kEnospc = 28;
// ERROR_DISK_FULL.
constexpr int kWinErrorDiskFull = 112;

constexpr std::size_t kMaxChain = 8;

template <std::size_t N>
bool contains_any(const std::string& text, const std::array<std::string_view, N>& markers) {
    return std::ranges::any_of(markers, [&text](std::string_view marker) {
        return text.find(marker) != std::string::npos;
    });
}

ErrorCause cause_of(const std::exception& error) {
    ErrorCause cause;
    cause.text = error.what();
    if (dynamic_cast<const ComponentCanceled*>(&error) != nullptr ||
        dynamic_cast<const download::DownloadCanceled*>(&error) != nullptr) {
        cause.text = "Model download canceled";
        return cause;
    }
    if (const auto* system = dynamic_cast<const std::system_error*>(&error)) {
        const std::error_code& code = system->code();
        if (code.category() == std::generic_category()) {
            cause.errno_value = code.value();
        } else if (code.category() == std::system_category()) {
#ifdef _WIN32
            cause.winerror = code.value();
#endif
            const std::error_condition condition = code.default_error_condition();
            if (condition.category() == std::generic_category()) {
                cause.errno_value = condition.value();
            }
        }
        if (code == std::errc::no_space_on_device) {
            cause.errno_value = kEnospc;
        }
    }
    return cause;
}

void walk(const std::exception_ptr& error, std::vector<ErrorCause>& chain) {
    if (!error || chain.size() >= kMaxChain) {
        return;
    }
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& caught) {
        chain.push_back(cause_of(caught));
        if (const auto* failed = dynamic_cast<const download::DownloadError*>(&caught);
            failed != nullptr && failed->failure() && chain.size() < kMaxChain) {
            ErrorCause reason;
            reason.text = failed->failure()->reason;
            chain.push_back(std::move(reason));
        }
        if (const auto* nested = dynamic_cast<const std::nested_exception*>(&caught);
            nested != nullptr && nested->nested_ptr()) {
            walk(nested->nested_ptr(), chain);
        }
    } catch (...) {
        chain.push_back(ErrorCause{});
    }
}

} // namespace

std::string_view access_decision_name(AccessDecision decision) {
    switch (decision) {
    case AccessDecision::LoadCached:
        return "load_cached";
    case AccessDecision::DownloadAllowed:
        return "download_allowed";
    case AccessDecision::NeedsConsent:
        return "needs_consent";
    case AccessDecision::BlockedByEnv:
        return "blocked_by_env";
    }
    return "needs_consent";
}

std::string_view consent_action_name(ConsentAction action) {
    switch (action) {
    case ConsentAction::Cancel:
        return "cancel";
    case ConsentAction::DownloadOnce:
        return "download_once";
    case ConsentAction::AlwaysAllow:
        return "always_allow";
    case ConsentAction::OpenSettings:
        return "open_settings";
    }
    return "cancel";
}

std::optional<std::string> format_download_size(std::string_view model_name) {
    const auto size_mb = model_download_size_mb(model_name);
    if (!size_mb) {
        return std::nullopt;
    }
    return format_download_mb(static_cast<double>(*size_mb));
}

std::vector<ErrorCause> error_chain(const std::exception_ptr& error) {
    std::vector<ErrorCause> chain;
    walk(error, chain);
    return chain;
}

std::string describe_model_download_error(std::string_view model_name,
                                          std::span<const ErrorCause> chain) {
    const std::span<const ErrorCause> causes = chain.first(std::min(chain.size(), kMaxChain));
    std::string joined;
    for (std::size_t i = 0; i < causes.size(); ++i) {
        if (i > 0) {
            joined += ' ';
        }
        joined += causes[i].text;
    }
    const std::string text = py::lower(joined);
    const std::string model(model_name);

    if (text.find("canceled") != std::string::npos || text.find("cancelled") != std::string::npos) {
        return "Download of '" + model + "' was canceled.";
    }
    const bool disk_full = std::ranges::any_of(causes,
                                               [](const ErrorCause& cause) {
                                                   return cause.errno_value == kEnospc ||
                                                          cause.winerror == kWinErrorDiskFull;
                                               }) ||
                           contains_any(text, kDiskFullMarkers);
    if (disk_full) {
        std::string needed;
        if (const auto size = format_download_size(model_name)) {
            needed = " (about " + py::lstrip(*size, "~") + ")";
        }
        return "Not enough disk space for '" + model + "'" + needed +
               ". Free up space, then retry in Downloads.";
    }
    const bool offline =
        std::ranges::any_of(causes, [](const ErrorCause& cause) { return cause.network; }) ||
        contains_any(text, kOfflineMarkers);
    if (offline) {
        return "Couldn't reach the download server for '" + model +
               "'. Check the internet connection, then retry in Downloads.";
    }
    return "Couldn't download '" + model + "' (details in " + std::string(identity().log_file) +
           "). Retry in Downloads.";
}

std::string describe_model_download_error(std::string_view model_name,
                                          const std::exception_ptr& error) {
    const std::vector<ErrorCause> chain = error_chain(error);
    return describe_model_download_error(model_name, chain);
}

CachedModelsSnapshot::CachedModelsSnapshot(const SpeechModelCache& cache, const Clock& clock)
    : CachedModelsSnapshot([&cache] { return cache.inventory(); }, clock) {}

CachedModelsSnapshot::CachedModelsSnapshot(Source source, const Clock& clock)
    : source_(std::move(source)), clock_(clock) {}

std::optional<CachedModelsSnapshot::Inventory> CachedModelsSnapshot::peek() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void CachedModelsSnapshot::invalidate() {
    std::lock_guard lock(mutex_);
    ++epoch_;
    snapshot_.reset();
    snapshot_at_ = {};
}

CachedModelsSnapshot::Inventory CachedModelsSnapshot::scan(double max_age_seconds) {
    const double max_age = std::max(0.0, max_age_seconds);
    while (true) {
        std::uint64_t scan_epoch = 0;
        {
            std::unique_lock lock(mutex_);
            const auto age = std::chrono::duration<double>(clock_.steady() - snapshot_at_);
            if (max_age > 0.0 && snapshot_ && age.count() <= max_age) {
                return *snapshot_;
            }
            if (in_progress_) {
                done_.wait(lock);
                if (snapshot_) {
                    return *snapshot_;
                }
                continue;
            }
            in_progress_ = true;
            scan_epoch = epoch_;
        }

        std::vector<CachedModelInfo> listed;
        try {
            listed = source_();
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                in_progress_ = false;
            }
            done_.notify_all();
            throw;
        }
        Inventory cached;
        for (CachedModelInfo& info : listed) {
            std::string repo = info.repo_id;
            cached.insert_or_assign(std::move(repo), std::move(info));
        }

        bool current = false;
        {
            std::lock_guard lock(mutex_);
            in_progress_ = false;
            current = scan_epoch == epoch_;
            if (current) {
                snapshot_ = cached;
                snapshot_at_ = clock_.steady();
            }
        }
        done_.notify_all();
        if (current) {
            return cached;
        }
        // A download or delete invalidated this scan while it read the disk: wake the waiters
        // and take a fresh one.
    }
}

HuggingFaceAccessCoordinator::HuggingFaceAccessCoordinator(SettingsStore& settings,
                                                           IsCached is_cached, const EnvReader& env)
    : settings_(settings), is_cached_(std::move(is_cached)), env_(env) {}

std::string HuggingFaceAccessCoordinator::policy() const {
    return load_hf_access_policy(settings_);
}

void HuggingFaceAccessCoordinator::set_policy(std::string_view policy) {
    save_hf_access_policy(settings_, policy);
}

void HuggingFaceAccessCoordinator::grant_once(std::string_view model_name) {
    {
        std::lock_guard lock(mutex_);
        grants_.emplace(model_name);
    }
    kLog.info("One-time download grant issued for '{}'", model_name);
}

AccessDecision HuggingFaceAccessCoordinator::evaluate_access(std::string_view model_name,
                                                             bool advisory) {
    if (is_cached_ && is_cached_(model_name)) {
        return AccessDecision::LoadCached;
    }
    if (is_hf_hub_offline_env_set(env_)) {
        return AccessDecision::BlockedByEnv;
    }
    {
        std::lock_guard lock(mutex_);
        if (const auto grant = grants_.find(model_name); grant != grants_.end()) {
            if (!advisory) {
                grants_.erase(grant);
            }
            return AccessDecision::DownloadAllowed;
        }
    }
    if (policy() == hf_access_policy::kAlways) {
        return AccessDecision::DownloadAllowed;
    }
    return AccessDecision::NeedsConsent;
}

bool HuggingFaceAccessCoordinator::begin_request(std::string_view model_name) {
    std::lock_guard lock(mutex_);
    if (active_.contains(model_name)) {
        kLog.debug("Request for '{}' already in flight", model_name);
        return false;
    }
    active_.emplace(model_name);
    return true;
}

void HuggingFaceAccessCoordinator::end_request(std::string_view model_name) {
    std::lock_guard lock(mutex_);
    if (const auto found = active_.find(model_name); found != active_.end()) {
        active_.erase(found);
    }
}

std::vector<std::string>
HuggingFaceAccessCoordinator::claim_batch(std::span<const std::string> model_names) {
    std::vector<std::string> claimed;
    for (const std::string& model_name : model_names) {
        if (std::ranges::find(claimed, model_name) != claimed.end() ||
            (is_cached_ && is_cached_(model_name))) {
            continue;
        }
        if (!begin_request(model_name)) {
            continue;
        }
        claimed.push_back(model_name);
    }
    return claimed;
}

int HuggingFaceAccessCoordinator::requests_in_flight() const {
    std::lock_guard lock(mutex_);
    return static_cast<int>(active_.size());
}

} // namespace fv::models
