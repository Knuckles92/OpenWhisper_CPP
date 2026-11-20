#pragma once

// The upload fixtures of OpenWhisper's controller tests (tests/test_application_controller.py
// FakeOpenAIBackend, _batch_request, _capturing_cleanup): a runtime whose speech service builds
// the fake local engine for "parakeet" and a fake cloud engine for "api", the dictation slot's
// backend reachable the way the composition reaches it, and a cleanup pass that records what it
// is asked.

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "base/utf8.h"
#include "dictation/batch.h"
#include "dictation/cleaner_port.h"
#include "dictation/scratchpad_port.h"
#include "engine/error.h"
#include "runtime_support.h"
#include "speech/service.h"
#include "speech/testing/fake_backend.h"

namespace fv::dictation::testing {

// What every FakeApiBackend of one test does. Affinity: thread-safe.
struct ApiScript {
    // A test sets this to make every file one this backend splits (MiB).
    std::optional<double> split_size_mb() const {
        std::lock_guard lock(mutex);
        return split;
    }
    void set_split_size_mb(std::optional<double> value) {
        std::lock_guard lock(mutex);
        split = value;
    }
    std::vector<std::string> paths() const {
        std::lock_guard lock(mutex);
        return decoded;
    }

    mutable std::mutex mutex;
    std::optional<double> split;
    // Reports "Transcribing 2 chunks..." while it splits, as the cloud engine does.
    bool report_steps = true;
    // Replaces the transcript of a file (its UTF-8 path); may throw.
    std::function<std::string(const std::string& path)> transcribe;
    std::vector<std::string> decoded;
};

// FakeOpenAIBackend: takes whole files; "api:<path>" for a file it does not split, "api chunks"
// (after its own progress step) for one it does.
class FakeApiBackend final : public speech::Backend {
public:
    explicit FakeApiBackend(std::shared_ptr<ApiScript> script) : script_(std::move(script)) {}

    std::string_view id() const override { return "api"; }
    speech::Capabilities capabilities() const override {
        speech::Capabilities capabilities;
        capabilities.windows = false;
        capabilities.live_preview = false;
        capabilities.accepts_files = true;
        capabilities.max_window_s = 0;
        capabilities.split_over_mb = 23.0;
        return capabilities;
    }
    speech::Readiness load(const CancelToken&) override {
        return {speech::ReadyState::Ready, "Groq (whisper-large-v3-turbo)", ""};
    }
    speech::DecodeResult decode(const speech::DecodeJob& job) override {
        const std::string path =
            base_utf8::path_to_utf8(std::get<std::filesystem::path>(job.input));
        std::function<std::string(const std::string&)> replace;
        {
            std::lock_guard lock(script_->mutex);
            script_->decoded.push_back(path);
            replace = script_->transcribe;
        }
        speech::DecodeResult result;
        if (replace) {
            result.transcript.text = replace(path);
            return result;
        }
        if (!large_file_size_mb(path)) {
            result.transcript.text = "api:" + path;
            return result;
        }
        if (script_->report_steps) {
            report_progress(job, "Transcribing 2 chunks...", true);
        }
        result.transcript.text = "api chunks";
        return result;
    }
    void release() override {}
    std::optional<double> large_file_size_mb(const std::filesystem::path&) const override {
        return script_->split_size_mb();
    }

private:
    std::shared_ptr<ApiScript> script_;
};

inline speech::BackendConfig api_config() {
    speech::BackendConfig config;
    config.backend = "api";
    return config;
}

// The engines one rig's speech service builds, and the backend the dictation slot holds now
// (what SpeechService's backend facade reaches).
struct Engines {
    Engines() : local(std::make_shared<speech::FakeEngine>(local_script())) {}

    static speech::FakeScript local_script() {
        speech::FakeScript script;
        script.texts = {"Hello there."};
        return script;
    }
    speech::BackendFactory factory() {
        return [this](const speech::BackendConfig& config) -> std::unique_ptr<speech::Backend> {
            std::unique_ptr<speech::Backend> backend;
            if (config.backend == "api") {
                backend = std::make_unique<FakeApiBackend>(api);
            } else {
                backend = local->factory()(config);
            }
            current = backend.get();
            return backend;
        };
    }
    // The dictation slot backend's large_file_size_mb.
    std::optional<double> large_file_size_mb(const std::filesystem::path& path) const {
        speech::Backend* backend = current.load();
        return backend != nullptr ? backend->large_file_size_mb(path) : std::nullopt;
    }

    std::shared_ptr<speech::FakeEngine> local;
    std::shared_ptr<ApiScript> api = std::make_shared<ApiScript>();
    std::atomic<speech::Backend*> current{nullptr};
};

// _capturing_cleanup: available, answering `reply`, recording each pass. The system prompt is
// composed as M4's prompt builder will: the batch description after the base prompt.
// Affinity: thread-safe.
class RecordingCleaner final : public TranscriptCleaner {
public:
    struct Call {
        std::string text;
        std::optional<std::string> batch_context;
        std::optional<double> timeout_s;
        std::string system_prompt;
    };

    explicit RecordingCleaner(std::string reply = "Cleaned.") : reply_(std::move(reply)) {}

    bool is_available(const Json&) override {
        if (on_available) {
            on_available();
        }
        return available.load();
    }
    CleanupResult cleanup(const CleanupRequest& request) override {
        Call call{request.text, request.batch_context, request.timeout_s,
                  request.batch_context
                      ? compose_batch_cleanup_prompt(kBasePrompt, *request.batch_context)
                      : std::string(kBasePrompt)};
        std::optional<std::string> fail;
        {
            std::lock_guard lock(mutex_);
            calls_.push_back(std::move(call));
            fail = error;
        }
        CleanupResult result;
        if (fail) {
            result.text = request.text;
            result.error = fail;
            return result;
        }
        result.text = reply_;
        result.provider = "openai";
        result.model = "gpt-4o-mini";
        result.level = "medium";
        return result;
    }
    std::vector<Call> calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

    static constexpr std::string_view kBasePrompt = "Clean up this transcript.";
    std::atomic<bool> available{true};
    // Set before use: every pass fails with it and returns its text.
    std::optional<std::string> error;
    // Set before use: runs as the runtime asks whether cleanup is available.
    std::function<void()> on_available;

private:
    std::string reply_;
    mutable std::mutex mutex_;
    std::vector<Call> calls_;
};

// A Scratchpad the test focuses; it records what it takes. Affinity: one thread.
class FakeScratchpad final : public ScratchpadPort {
public:
    bool is_focused() const override { return focused; }
    bool insert(std::string text) override {
        inserted.push_back(std::move(text));
        return true;
    }
    bool focused = false;
    std::vector<std::string> inserted;
};

// A rig over Engines, with what the upload tests read off the runtime's signals.
struct UploadRig {
    explicit UploadRig(Json settings = paste_settings()) : rig(options(std::move(settings))) {
        auto& runtime = *rig.runtime;
        links.push_back(runtime.large_file_detected.connect(
            rig.ui, [this](double mb) { large_file_states.push_back(mb); }));
        links.push_back(runtime.batch_progress.connect(
            rig.ui, [this](int position, int total, std::string name) {
                batch_progress_events.emplace_back(position, total, std::move(name));
            }));
        links.push_back(runtime.batch_item_finished.connect(
            rig.ui, [this](int position, bool ok, std::string text) {
                batch_item_events.emplace_back(position, ok);
                batch_item_transcripts.push_back(std::move(text));
            }));
        links.push_back(
            runtime.batch_completed.connect(rig.ui, [this](BatchResult) { ++batches_completed; }));
    }
    ~UploadRig() { links.clear(); }
    UploadRig(const UploadRig&) = delete;
    UploadRig& operator=(const UploadRig&) = delete;

    RigOptions options(Json settings) {
        RigOptions made;
        made.settings = std::move(settings);
        made.factory = engines.factory();
        made.hooks = [this](RuntimeHooks& hooks) {
            hooks.large_file_size_mb = [this](const std::filesystem::path& path) {
                return engines.large_file_size_mb(path);
            };
        };
        return made;
    }

    // Selects the cloud engine, as the API backend becoming current_backend does.
    void use_api(std::optional<double> split_size_mb = std::nullopt) {
        engines.api->set_split_size_mb(split_size_mb);
        rig.settings.update(Json{{"selected_model", "api"}});
        rig.load(api_config());
    }
    // _batch_request: a real WAV per name in the data root, each 10 s long by its preview.
    BatchUploadRequest batch_request(const std::vector<std::string>& names,
                                     BatchRelation relation = BatchRelation::Separate,
                                     std::string custom_instructions = {},
                                     bool custom_combine = false) {
        BatchUploadRequest request;
        for (const std::string& name : names) {
            request.items.push_back(BatchItem{base_utf8::path_to_utf8(rig.wav(name)), 10.0});
        }
        request.relation = relation;
        request.custom_instructions = std::move(custom_instructions);
        request.custom_combine = custom_combine;
        return request;
    }
    // _run_batch, and the History save after it.
    void run_batch() { rig.run_workers(); }

    // The History entries in the order they were saved (history.saved's order; History lists
    // newest first, and entries saved within one second tie).
    std::vector<HistoryEntry> saved_entries() {
        const auto all = rig.entries();
        std::vector<HistoryEntry> ordered;
        for (const auto& event : rig.events.of("history.saved")) {
            const std::string id = event.data.value("record_id", "");
            for (const auto& entry : all) {
                if (entry.id == id) {
                    ordered.push_back(entry);
                }
            }
        }
        return ordered;
    }
    std::vector<std::string> source_names() {
        std::vector<std::string> names;
        for (const auto& entry : saved_entries()) {
            names.push_back(entry.source_name.value_or(""));
        }
        return names;
    }
    std::vector<std::string> texts() {
        std::vector<std::string> out;
        for (const auto& entry : saved_entries()) {
            out.push_back(entry.text);
        }
        return out;
    }
    std::vector<std::string> overlays() const { return rig.trace.with_prefix("overlay:"); }
    std::vector<std::string> error_dialogs() const {
        return rig.trace.with_prefix("error_dialog:");
    }
    std::vector<std::string> transcripts() const { return rig.trace.with_prefix("transcript:"); }

    Engines engines;
    RuntimeRig rig;
    std::vector<double> large_file_states;
    std::vector<std::tuple<int, int, std::string>> batch_progress_events;
    std::vector<std::pair<int, bool>> batch_item_events;
    std::vector<std::string> batch_item_transcripts;
    int batches_completed = 0;
    std::vector<Subscription> links;
};

} // namespace fv::dictation::testing
