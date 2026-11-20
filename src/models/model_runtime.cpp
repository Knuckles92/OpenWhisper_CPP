#include "models/model_runtime.h"

#include <exception>
#include <format>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "base/log.h"
#include "models/catalog.h"
#include "models/component_runtime.h"
#include "models/hf_access_policy.h"
#include "models/progress_throttle.h"
#include "models/runtime_offer.h"
#include "models/runtime_resolver.h"
#include "models/speech_catalog.h"
#include "settings/keys.h"

namespace fv::models {
namespace {

constexpr log::Logger kLog("services.runtime.models");

constexpr std::string_view kBackend = "parakeet";
constexpr std::string_view kEngineRefusal = "End the meeting before changing the engine";
constexpr std::string_view kModelInUse = "Model is in use — switch models first";

// resolve_model_repo for a speech model: its pinned repository; any other name stands for itself.
std::string model_repo(std::string_view model_name) {
    if (model(model_name) != nullptr) {
        try {
            return artifacts(model_name).at("repo").get<std::string>();
        } catch (const std::exception&) {
            // A model without pins compares by its name.
        }
    }
    return std::string(model_name);
}

} // namespace

ModelRuntime::ModelRuntime(EngineControl& engine, SpeechModelCache& cache,
                           HuggingFaceAccessCoordinator& access, ComponentCoordinator& components,
                           SettingsStore& settings, Executor& ui, Executor& work,
                           const Clock& clock, ModelRuntimeOptions options)
    : engine_(engine), cache_(cache), access_(access), components_(components), settings_(settings),
      ui_(ui), work_(work), clock_(clock), options_(std::move(options)) {}

ModelRuntime::~ModelRuntime() {
    alive_.reset();
}

template <class F> void ModelRuntime::post_work(F task) {
    std::weak_ptr<int> alive = alive_;
    work_.post([alive, task = std::move(task)]() mutable {
        if (alive.lock()) {
            task();
        }
    });
}

template <class F> void ModelRuntime::post_ui(F task) {
    std::weak_ptr<int> alive = alive_;
    ui_.post([alive, task = std::move(task)]() mutable {
        if (alive.lock()) {
            task();
        }
    });
}

Json ModelRuntime::settings_snapshot() const {
    return *settings_.snapshot();
}

const Host& ModelRuntime::host() const {
    return options_.host ? *options_.host : components_.store().host();
}

bool ModelRuntime::offline() const {
    return is_hf_hub_offline_env_set(options_.env != nullptr ? *options_.env : system_env());
}

std::string ModelRuntime::selected_local_model() const {
    if (engine_.selected_backend() != kBackend) {
        return {};
    }
    return selected_model(settings_snapshot(), kBackend);
}

void ModelRuntime::forget_runtime_prompts() {
    runtime_prompted_.clear();
}

// Switching to "always" authorizes downloads without asking, so a missing selected model is
// fetched at once; the other policies apply at the next request.
void ModelRuntime::on_hf_policy_changed(const std::string& policy) {
    if (policy == hf_access_policy::kAlways && engine_.selected_backend() == kBackend) {
        ensure_selected_model_available();
    }
}

void ModelRuntime::ensure_selected_model_available() {
    const std::string model_name = selected_local_model();
    if (model_name.empty() || !engine_.loaded_model_key().empty()) {
        return;
    }
    if (cache_.is_cached(model_name)) {
        // The load failed for another reason (the runtime, the hardware); a download would not
        // help.
        return;
    }
    request_download(model_name, true);
}

std::optional<std::string> ModelRuntime::missing_local_runtime() const {
    if (engine_.selected_backend() != kBackend) {
        return std::nullopt;
    }
    const Json settings = settings_snapshot();
    if (!keys::fv_runtime_dir.read(settings).empty()) {
        // An explicit runtime folder stands in for the component (resolve_parakeet), so none is
        // missing.
        return std::nullopt;
    }
    const Host& here = host();
    // The runtime the engine resolves (resolve_parakeet's runtime half): a chosen runtime build
    // fixes it, else the device setting does.
    const std::string runtime = keys::fv_speech_runtime.read(settings);
    std::string component;
    if (runtime == "cpu" || runtime == "cuda") {
        component = runtime_id(kBackend, runtime, here);
    } else {
        component = resolve_runtime(kBackend, selected_device(settings, kBackend), here).first;
    }
    if (component.empty() || here.installed(component)) {
        return std::nullopt;
    }
    const auto entry = catalog_entry(component, here);
    if (!entry || check_compatibility(*entry, here)) {
        return std::nullopt;
    }
    return component;
}

std::optional<std::string> ModelRuntime::loaded_local_model() const {
    std::string loaded = engine_.loaded_model_key();
    if (loaded.empty()) {
        return std::nullopt;
    }
    return loaded;
}

void ModelRuntime::request_download(const std::string& model_name, bool load_into_engine) {
    if (!load_into_engine) {
        // A fetch from Downloads asks about the model's runtime again.
        if (const auto component = missing_runtime(model_name, settings_snapshot(), host())) {
            runtime_prompted_.erase(*component);
        }
    }
    evaluate_request(model_name, load_into_engine);
}

void ModelRuntime::evaluate_request(const std::string& model_name, bool load_into_engine) {
    if (!access_.begin_request(model_name)) {
        return; // a consent question or a download is already in flight
    }
    AccessDecision decision = AccessDecision::NeedsConsent;
    try {
        // Advisory: the worker makes the grant-consuming evaluation before any network.
        decision = access_.evaluate_access(model_name, true);
    } catch (...) {
        access_.end_request(model_name);
        throw;
    }

    const auto ask = [&](bool env_blocked) {
        consents_[model_name] = PendingConsent{env_blocked, load_into_engine};
        if (hf_consent_requested.subscriber_count() == 0) {
            answer_consent(model_name, ConsentAction::Cancel);
            return;
        }
        hf_consent_requested.emit(model_name, env_blocked, load_into_engine);
    };

    switch (decision) {
    case AccessDecision::LoadCached:
        if (load_into_engine) {
            start_task(model_name);
        } else {
            // Downloads' row was stale: the files are already here.
            access_.end_request(model_name);
            model_cache_changed.emit();
            prompt_for_runtime(model_name);
        }
        break;
    case AccessDecision::DownloadAllowed:
        start_task(model_name);
        break;
    case AccessDecision::BlockedByEnv:
        ask(true);
        break;
    case AccessDecision::NeedsConsent:
        ask(false);
        break;
    }
}

// _on_hf_consent_requested after the dialog: the request slot the requester claimed is handed to
// the download worker or released here.
void ModelRuntime::answer_consent(const std::string& model_name, ConsentAction action) {
    const auto found = consents_.find(model_name);
    if (found == consents_.end()) {
        return;
    }
    const PendingConsent pending = found->second;
    consents_.erase(found);

    if (pending.env_blocked) {
        access_.end_request(model_name);
        status.emit(std::format("Model '{}' unavailable — downloads disabled by HF_HUB_OFFLINE",
                                model_name));
        return;
    }
    switch (action) {
    case ConsentAction::DownloadOnce:
        access_.grant_once(model_name);
        start_task(model_name);
        break;
    case ConsentAction::AlwaysAllow:
        access_.set_policy(hf_access_policy::kAlways);
        start_task(model_name);
        break;
    case ConsentAction::OpenSettings:
        access_.end_request(model_name);
        hf_settings_requested.emit();
        break;
    case ConsentAction::Cancel:
        // No network activity. OpenWhisper's revert of a declined selection is Whisper's only.
        access_.end_request(model_name);
        status.emit(std::format("Model '{}' is unavailable — download declined", model_name));
        break;
    }
}

// _start_hf_model_task for a model of MODELS: never loaded by the worker and never marking the
// engine busy. The runtime question comes first; the download starts once it is answered.
void ModelRuntime::start_task(const std::string& model_name) {
    ask_runtime(model_name, [this, model_name] {
        model_download_started.emit(model_name);
        post_work([this, model_name] { model_worker(model_name); });
    });
}

void ModelRuntime::prompt_for_runtime(const std::string& model_name) {
    ask_runtime(model_name, {});
}

// _prompt_for_model_runtime. On Auto with no runtime yet, an NVIDIA GPU that can run the model
// is offered first, with the CPU runtime Auto falls back to as the alternative; installing
// either ends the question.
void ModelRuntime::ask_runtime(const std::string& model_name, std::function<void()> then) {
    if (prompt_) {
        // A question is open: what follows this one waits for that answer as well.
        if (then) {
            prompt_->then.push_back(std::move(then));
        }
        return;
    }
    const auto proceed = [&then] {
        if (then) {
            then();
        }
    };
    if (offline() || engine_.job_active() || engine_.meeting_active()) {
        proceed();
        return;
    }
    const Json settings = settings_snapshot();
    const auto component = missing_runtime(model_name, settings, host());
    if (!component || runtime_prompted_.contains(*component) ||
        components_.is_installing(*component)) {
        proceed();
        return;
    }
    const auto gpu_component = gpu_runtime_offer(model_name, settings, host());
    if (gpu_component && components_.is_installing(*gpu_component)) {
        proceed();
        return;
    }
    runtime_prompted_.insert(*component);
    prompt_ = PendingPrompt{model_name, *component, gpu_component.value_or(""), {}};
    if (then) {
        prompt_->then.push_back(std::move(then));
    }
    if (gpu_component) {
        if (gpu_runtime_offer_requested.subscriber_count() == 0) {
            finish_prompt(std::nullopt);
            return;
        }
        gpu_runtime_offer_requested.emit(model_name, *gpu_component, *component);
    } else {
        if (runtime_consent_requested.subscriber_count() == 0) {
            finish_prompt(std::nullopt);
            return;
        }
        runtime_consent_requested.emit(model_name, *component);
    }
}

void ModelRuntime::answer_runtime(const std::string& component_id, bool install) {
    if (!prompt_ || !prompt_->gpu_component.empty() || prompt_->component != component_id) {
        return;
    }
    finish_prompt(install ? std::optional<std::string>(component_id) : std::nullopt);
}

void ModelRuntime::answer_gpu_offer(const std::string& model_name,
                                    std::optional<std::string> component_id) {
    if (!prompt_ || prompt_->gpu_component.empty() || prompt_->model != model_name) {
        return;
    }
    finish_prompt(component_id);
}

void ModelRuntime::finish_prompt(const std::optional<std::string>& chosen) {
    PendingPrompt pending = std::move(*prompt_);
    prompt_.reset();
    if (chosen && !chosen->empty()) {
        downloads_requested.emit(*chosen);
        if (component_runtime_ != nullptr) {
            component_runtime_->request_install(*chosen);
        }
    }
    for (auto& then : pending.then) {
        then();
    }
}

// _on_model_download_finished: the runtime question, then the selected engine loads the model it
// was missing. A remote engine runs the host's model, which a download here does not change.
void ModelRuntime::on_download_finished(const std::string& model_name, bool success) {
    if (!success) {
        return;
    }
    ask_runtime(model_name, [this, model_name] {
        if (selected_local_model() != model_name) {
            return;
        }
        if (engine_.meeting_active()) {
            // A load mid-meeting would sit a second copy of the weights beside the meeting's.
            kLog.info("Engine load refused: a meeting owns the engine");
            status.emit(std::string(kEngineRefusal));
            return;
        }
        engine_.reload_engine();
    });
}

void ModelRuntime::emit_finished(const std::string& model_name, bool success) {
    model_download_finished.emit(model_name, success);
    post_ui([this, model_name, success] { on_download_finished(model_name, success); });
    if (success) {
        model_cache_changed.emit();
    }
}

// _hf_model_worker with load_into_engine False (every model of MODELS).
void ModelRuntime::model_worker(const std::string& model_name) {
    bool success = false;
    try {
        if (!options_.shutdown.cancelled()) {
            success = download_to_cache(model_name);
        }
    } catch (const std::exception& error) {
        kLog.error_with(error, std::format("Model download/load failed for '{}': {}", model_name,
                                           error.what()));
        status.emit(describe_model_download_error(model_name, std::current_exception()));
    }
    access_.end_request(model_name);
    emit_finished(model_name, success);
}

// _download_model_to_cache: evaluates access (consuming a grant) and downloads. The caller owns
// the request slot and the started and finished signals. True when the files are present.
bool ModelRuntime::download_to_cache(const std::string& model_name) {
    if (options_.shutdown.cancelled()) {
        return false;
    }
    const AccessDecision decision = access_.evaluate_access(model_name);
    if (decision != AccessDecision::LoadCached && decision != AccessDecision::DownloadAllowed) {
        kLog.warning("Fetch of '{}' aborted: access decision {}", model_name,
                     access_decision_name(decision));
        status.emit(std::format("Model '{}' is unavailable", model_name));
        return false;
    }
    // The access check reads the cache; a shutdown during it starts no transfer.
    if (options_.shutdown.cancelled()) {
        return false;
    }
    if (decision == AccessDecision::DownloadAllowed) {
        status.emit(std::format("Downloading model '{}' from Hugging Face...", model_name));
        ProgressThrottle throttle(clock_);
        cache_.download(
            model_name,
            [this, &throttle, &model_name](std::int64_t done, std::int64_t total) {
                if (throttle.should_emit(done, total)) {
                    model_download_progress.emit(model_name, done, total);
                }
            },
            options_.shutdown);
        if (options_.snapshot != nullptr) {
            options_.snapshot->invalidate();
        }
        status.emit(std::format("Model '{}' downloaded", model_name));
    }
    return true;
}

void ModelRuntime::request_batch(const std::vector<std::string>& models) {
    if (offline()) {
        status.emit("Downloads are disabled by HF_HUB_OFFLINE");
        return;
    }
    if (access_.requests_in_flight() > 0) {
        // One download at a time, even when the other request came from elsewhere.
        status.emit("Another model request is in progress — try again when it finishes");
        return;
    }
    std::vector<std::string> pending = access_.claim_batch(models);
    if (pending.empty()) {
        // Everything is cached or in flight already; a rescan corrects stale rows.
        model_cache_changed.emit();
        return;
    }
    batch_stop_requested_ = false;
    batch_download_planned.emit(pending);
    post_work([this, pending] { batch_worker(pending); });
}

void ModelRuntime::stop_batch() {
    batch_stop_requested_ = true;
}

// _hf_batch_worker: each model is granted and announced right before its own download. claim_batch
// gave this worker one request slot per model: processed models are released in the loop, the
// ones a stop left unreached after it.
void ModelRuntime::batch_worker(const std::vector<std::string>& models) {
    std::size_t processed = 0;
    int succeeded = 0;
    for (const std::string& model_name : models) {
        if (batch_stop_requested_ || options_.shutdown.cancelled()) {
            break;
        }
        access_.grant_once(model_name);
        model_download_started.emit(model_name);
        bool success = false;
        try {
            success = download_to_cache(model_name);
        } catch (const std::exception& error) {
            kLog.error_with(
                error, std::format("Model download failed for '{}': {}", model_name, error.what()));
            status.emit(describe_model_download_error(model_name, std::current_exception()));
        }
        access_.end_request(model_name);
        emit_finished(model_name, success);
        ++processed;
        if (success) {
            ++succeeded;
        }
    }
    const bool stopped = batch_stop_requested_.exchange(false);
    for (std::size_t index = processed; index < models.size(); ++index) {
        access_.end_request(models[index]);
    }
    if (stopped) {
        kLog.info("Batch download stopped after {} of {} models", succeeded, models.size());
    }
    batch_download_finished.emit(succeeded, static_cast<int>(models.size()));
}

bool ModelRuntime::model_in_use(const std::string& model_name) const {
    const std::string loaded = engine_.loaded_model_key();
    return !loaded.empty() && model_repo(loaded) == model_repo(model_name);
}

// request_model_delete: the engine memory-maps the loaded model, so its files are never removed;
// the request slot also keeps a download of the same model out.
void ModelRuntime::request_delete(const std::string& model_name) {
    if (model(model_name) != nullptr && engine_.meeting_active()) {
        model_deleted.emit(model_name, false, "End the meeting before removing speech models");
        return;
    }
    if (model_in_use(model_name)) {
        model_deleted.emit(model_name, false, std::string(kModelInUse));
        return;
    }
    if (!access_.begin_request(model_name)) {
        model_deleted.emit(model_name, false, "A download for this model is in progress");
        return;
    }
    post_work([this, model_name] { delete_worker(model_name); });
}

void ModelRuntime::delete_worker(const std::string& model_name) {
    try {
        const auto remove = [this, &model_name] {
            cache_.remove(model_name);
        };
        if (model(model_name) != nullptr) {
            if (!engine_.with_model_unused(model_name, remove)) {
                throw std::runtime_error(std::string(kModelInUse));
            }
        } else {
            remove();
        }
        if (options_.snapshot != nullptr) {
            options_.snapshot->invalidate();
        }
        status.emit(std::format("Model '{}' deleted", model_name));
        model_deleted.emit(model_name, true, "");
        model_cache_changed.emit();
    } catch (const std::system_error& error) {
        kLog.error("Model delete failed for '{}': {}", model_name, error.what());
        model_deleted.emit(model_name, false, "Files are in use by another process");
    } catch (const std::exception& error) {
        kLog.error_with(error,
                        std::format("Model delete failed for '{}': {}", model_name, error.what()));
        model_deleted.emit(model_name, false, error.what());
    }
    access_.end_request(model_name);
}

} // namespace fv::models
