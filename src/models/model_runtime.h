#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/env.h"
#include "base/executor.h"
#include "base/signal.h"
#include "models/coordinator.h"
#include "models/engine_control.h"
#include "models/hf_access.h"
#include "models/speech_model_cache.h"
#include "settings/store.h"

// Local speech models: download consent, downloads, deletes and the runtime prompt
// (services/runtime/models.py). Requests arrive on the UI executor; downloads and deletes run on
// the downloads strand and report through the signals.
//
// OpenWhisper's dialogs are modal and answer synchronously; here each question is a signal and
// its answer a call: hf_consent_requested is answered by answer_consent,
// runtime_consent_requested by answer_runtime and gpu_runtime_offer_requested by
// answer_gpu_offer. What OpenWhisper ran after a dialog closed waits for the answer: a download
// that asked for its runtime first starts once the runtime question is answered. A question
// nobody listens to is answered "Later" (or Cancel) at once.
//
// Only the Parakeet family is downloadable, so every download is fetch-only (OpenWhisper's
// load_into_engine is False for every model in MODELS): a finished download of the selected
// model reloads the engine, unless a meeting runs. Whisper's declined-download revert, its GPU
// fallback and the "Use this GPU" offer are not ported.
namespace fv::models {

class ComponentRuntime;

struct ModelRuntimeOptions {
    // Downloads stop when the app shuts down (the controller's _shutdown_cancel).
    CancelToken shutdown;
    // HF_HUB_OFFLINE; nullptr: system_env().
    const EnvReader* env = nullptr;
    // The shared inventory, discarded after a download or delete; nullptr: none to discard.
    CachedModelsSnapshot* snapshot = nullptr;
    // The computer the runtime decisions read (platform, GPU, installed components); empty:
    // the coordinator's store's host.
    std::optional<Host> host;
};

// Affinity: UI actor on `ui`; the workers run on `work`. The owner cancels `shutdown` and shuts
// the downloads strand down before destroying this object.
class ModelRuntime {
public:
    ModelRuntime(EngineControl& engine, SpeechModelCache& cache,
                 HuggingFaceAccessCoordinator& access, ComponentCoordinator& components,
                 SettingsStore& settings, Executor& ui, Executor& work, const Clock& clock,
                 ModelRuntimeOptions options = {});
    ~ModelRuntime();
    ModelRuntime(const ModelRuntime&) = delete;
    ModelRuntime& operator=(const ModelRuntime&) = delete;

    // Where an accepted runtime question installs the runtime (controller.components).
    // Without one, an accepted answer only asks Downloads to open on it.
    void set_component_runtime(ComponentRuntime* components) { component_runtime_ = components; }

    // request_model_download (load_into_engine false: Downloads' fetch-only download) or the
    // download half of ensure_local_model_available (true: the selected model's own flow). Both
    // go through the coordinator's request slot, policy and grants; the flag only travels to
    // the consent question.
    void request_download(const std::string& model, bool load_into_engine);
    // request_model_batch_download: Downloads' confirmed queue, fetched strictly in sequence
    // with no per-model consent ("Downloads are disabled by HF_HUB_OFFLINE", "Another model
    // request is in progress — try again when it finishes").
    void request_batch(const std::vector<std::string>& models);
    // cancel_model_batch_download: the queue stops once the model downloading now finishes.
    void stop_batch();
    // request_model_delete: refused during a meeting, for the loaded model and for a model with
    // a request in flight; the files go on the downloads strand.
    void request_delete(const std::string& model);
    // ensure_local_model_available: when the selected Parakeet engine is not loaded because its
    // model is missing, the model is fetched (or consent asked) and then loaded.
    void ensure_selected_model_available();
    // A Settings change of the Hugging Face policy: "always" fetches a missing selected model.
    void on_hf_policy_changed(const std::string& policy);
    // Ask about a missing runtime again (the engine selection changed).
    void forget_runtime_prompts();
    // _prompt_for_model_runtime: asks once per runtime per session for the runtime `model`
    // needs, the GPU runtime Auto would pick first when it applies. Nothing is asked offline,
    // during a meeting or a speech job (recording, transcription, reload), for a runtime being
    // installed, or while another runtime question is open.
    void prompt_for_runtime(const std::string& model);

    // The answers.
    void answer_consent(const std::string& model, ConsentAction action);
    // The required-runtime question: install `component_id` now or later.
    void answer_runtime(const std::string& component_id, bool install);
    // The GPU runtime offer: the runtime chosen (the GPU or the CPU one), or nullopt for Later.
    void answer_gpu_offer(const std::string& model, std::optional<std::string> component_id);

    // get_missing_local_runtime: the runtime the selected Parakeet engine needs and lacks.
    std::optional<std::string> missing_local_runtime() const;
    // get_loaded_local_model: the loaded model, whose files cannot be deleted.
    std::optional<std::string> loaded_local_model() const;

    // (model, env_blocked, load_into_engine): show the download consent dialog.
    Signal<std::string, bool, bool> hf_consent_requested;
    // (model, component): show the required-runtime dialog.
    Signal<std::string, std::string> runtime_consent_requested;
    // (model, GPU runtime, CPU runtime): offer the GPU runtime beside the CPU one.
    Signal<std::string, std::string, std::string> gpu_runtime_offer_requested;
    Signal<std::string> model_download_started;
    // (model, done bytes, total bytes); throttled at the source.
    Signal<std::string, std::int64_t, std::int64_t> model_download_progress;
    Signal<std::string, bool> model_download_finished;
    // (model, success, message).
    Signal<std::string, bool, std::string> model_deleted;
    Signal<> model_cache_changed;
    Signal<std::vector<std::string>> batch_download_planned;
    // (succeeded, planned).
    Signal<int, int> batch_download_finished;
    // The status line (the controller's status_update).
    Signal<std::string> status;
    // The consent dialog's "Open Settings": Settings opens on the Hugging Face policy.
    Signal<> hf_settings_requested;
    // An accepted runtime question: Downloads opens on that component (ui.open_downloads).
    Signal<std::string> downloads_requested;

private:
    struct PendingConsent {
        bool env_blocked = false;
        bool load_into_engine = false;
    };
    struct PendingPrompt {
        std::string model;
        std::string component;     // the runtime missing_runtime names
        std::string gpu_component; // the GPU runtime offered beside it, or ""
        std::vector<std::function<void()>> then;
    };

    Json settings_snapshot() const;
    const Host& host() const;
    bool offline() const;
    // The selected Parakeet model, or "" when another engine is selected.
    std::string selected_local_model() const;
    void evaluate_request(const std::string& model, bool load_into_engine);
    void start_task(const std::string& model);
    void ask_runtime(const std::string& model, std::function<void()> then);
    void finish_prompt(const std::optional<std::string>& chosen);
    void on_download_finished(const std::string& model, bool success);
    void model_worker(const std::string& model);
    bool download_to_cache(const std::string& model);
    void batch_worker(const std::vector<std::string>& models);
    void delete_worker(const std::string& model);
    void emit_finished(const std::string& model, bool success);
    bool model_in_use(const std::string& model) const;
    template <class F> void post_work(F task);
    template <class F> void post_ui(F task);

    EngineControl& engine_;
    SpeechModelCache& cache_;
    HuggingFaceAccessCoordinator& access_;
    ComponentCoordinator& components_;
    SettingsStore& settings_;
    Executor& ui_;
    Executor& work_;
    const Clock& clock_;
    ModelRuntimeOptions options_;
    ComponentRuntime* component_runtime_ = nullptr;

    // Set on the UI executor, read by the batch worker between models.
    std::atomic<bool> batch_stop_requested_{false};
    std::set<std::string, std::less<>> runtime_prompted_;
    std::map<std::string, PendingConsent, std::less<>> consents_;
    std::optional<PendingPrompt> prompt_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace fv::models
