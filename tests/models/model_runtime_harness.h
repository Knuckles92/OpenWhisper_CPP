#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/hash.h"
#include "base/subscription.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "base/testing/map_env_reader.h"
#include "http/testing/fake_http_client.h"
#include "models/component_leases.h"
#include "models/component_runtime.h"
#include "models/component_store.h"
#include "models/coordinator.h"
#include "models/hf_access.h"
#include "models/hf_access_policy.h"
#include "models/model_runtime.h"
#include "models/speech_catalog.h"
#include "models/speech_model_cache.h"
#include "models/testing/fake_engine_control.h"
#include "models/testing/fake_openwhisper.h"
#include "models_test_support.h"
#include "settings/testing/memory_store.h"

// The model and component runtimes over fakes, as OpenWhisper's TestApplicationController builds
// its controller: an in-memory settings file, a fake engine, the downloads and components strands
// as manual executors, a fake HTTP client serving a small pinned model and the runtime archive,
// and a presenter that records every signal on the UI executor and answers the questions it is
// told to answer.
namespace fv::models::test {

inline const std::string kParakeet = "parakeet-v3";
inline const std::string kOrukeet = "orukeet-v0.1";
inline const std::string kCpuRuntime = "asr-nvidia-cpu";
inline const std::string kCudaRuntime = "asr-nvidia-cuda";
inline const std::string kWeights = "verified model weights";

struct RuntimeHarness {
    TempDir temp;
    fv::http::FakeHttpClient client;
    fv::MapEnvReader env;
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    fv::ManualExecutor ui;
    fv::ManualExecutor work;
    fv::ManualExecutor component_strand;
    FakeEngineControl engine;
    ComponentLeases leases;
    fv::CancelSource shutdown;
    std::string archive =
        stored_zip({{"bin/nemo_speech_asr_c.dll", "runtime"}, {"bin/ggml.dll", "ggml"}});
    Json catalog;

    std::unique_ptr<ComponentStore> store;
    std::unique_ptr<ComponentCoordinator> coordinator;
    std::unique_ptr<SpeechModelCache> cache;
    std::unique_ptr<HuggingFaceAccessCoordinator> access;
    std::unique_ptr<ModelRuntime> runtime;
    std::unique_ptr<ComponentRuntime> components;

    // Called with the model on every cache check the access coordinator makes.
    std::function<void(std::string_view)> on_cache_check;
    // Replaces one file's download (the cache's download_file); empty: the fake client serves it.
    SpeechModelCacheOptions cache_options;

    // The presenter's answers; unset leaves the question open. The runtime questions are
    // answered "Later" unless a test says otherwise.
    std::optional<ConsentAction> consent_answer;
    std::optional<bool> runtime_answer = false;
    std::optional<std::optional<std::string>> gpu_answer = std::optional<std::string>();

    // Every signal and engine command, in the order the UI executor saw them.
    Json trace = Json::array();
    std::vector<std::string> statuses;
    std::vector<fv::Subscription> links;

    // A Windows computer; `card` is its NVIDIA GPU, if any.
    explicit RuntimeHarness(std::optional<NvidiaGpu> card = std::nullopt) {
        client.set_handler([this](const fv::http::Request& request) {
            if (request.url.find(".zip") != std::string::npos) {
                return fv::http::FakeHttpClient::serve(request, archive);
            }
            return fv::http::FakeHttpClient::serve(request, kWeights);
        });
        const Json entry_cpu = runtime_entry("cpu");
        const Json entry_cuda = runtime_entry("cuda");
        catalog = Json{{"schema", 2},
                       {"components",
                        {{kCpuRuntime, {{"platforms", {{"win_amd64", entry_cpu}}}}},
                         {kCudaRuntime, {{"platforms", {{"win_amd64", entry_cuda}}}}}}}};

        ComponentStoreOptions store_options;
        store_options.host = host_for(std::string(platform::kWinAmd64), card);
        store_options.leases = &leases;
        store_options.free_bytes = [](const fs::path&) {
            return std::uint64_t{1} << 40;
        };
        store = std::make_unique<ComponentStore>(temp.path() / "components", client, store_options);
        ComponentCoordinator::Options coordinator_options;
        coordinator_options.catalog = &catalog;
        coordinator = std::make_unique<ComponentCoordinator>(*store, coordinator_options);

        cache_options.env = &env;
        cache_options.artifacts = [](std::string_view key) {
            Json spec = testing::small_model_spec("weights.gguf", kWeights);
            spec["repo"] = "test/" + std::string(key);
            return spec;
        };
    }

    // Builds the runtimes; a test changes the options above first.
    void build() {
        cache = std::make_unique<SpeechModelCache>(temp.path() / "speech-models", client,
                                                   cache_options);
        access = std::make_unique<HuggingFaceAccessCoordinator>(
            settings,
            [this](std::string_view name) {
                if (on_cache_check) {
                    on_cache_check(name);
                }
                return model(name) != nullptr && cache->is_cached(name);
            },
            env);
        ModelRuntimeOptions options;
        options.shutdown = shutdown.token();
        options.env = &env;
        runtime = std::make_unique<ModelRuntime>(engine, *cache, *access, *coordinator, settings,
                                                 ui, work, clock, options);
        components =
            std::make_unique<ComponentRuntime>(engine, *coordinator, ui, component_strand, clock);
        runtime->set_component_runtime(components.get());
        engine.on_call = [this](const std::string& call) {
            trace.push_back(Json::array({call}));
        };
        connect();
    }

    Json runtime_entry(std::string version) const {
        return Json{{"version", std::move(version)},
                    {"component_api", kComponentApi},
                    {"platform", "win_amd64"},
                    {"install_bytes", 1000},
                    {"archives", Json::array({Json{{"name", "runtime.zip"},
                                                   {"url", "https://example.invalid/runtime.zip"},
                                                   {"sha256", fv::sha256_hex(archive)},
                                                   {"size_bytes", archive.size()},
                                                   {"extract", "zip"}}})}};
    }

    void connect() {
        auto& r = *runtime;
        links.push_back(r.hf_consent_requested.connect(ui, [this](std::string model_name,
                                                                  bool env_blocked, bool load) {
            trace.push_back(Json::array({"hf_consent_requested", model_name, env_blocked, load}));
            if (consent_answer) {
                runtime->answer_consent(model_name, *consent_answer);
            }
        }));
        links.push_back(r.runtime_consent_requested.connect(
            ui, [this](std::string model_name, std::string component) {
                trace.push_back(Json::array({"runtime_consent_requested", model_name, component}));
                if (runtime_answer) {
                    runtime->answer_runtime(component, *runtime_answer);
                }
            }));
        links.push_back(r.gpu_runtime_offer_requested.connect(
            ui, [this](std::string model_name, std::string gpu, std::string cpu) {
                trace.push_back(Json::array({"gpu_runtime_offer_requested", model_name, gpu, cpu}));
                if (gpu_answer) {
                    runtime->answer_gpu_offer(model_name, *gpu_answer);
                }
            }));
        links.push_back(r.model_download_started.connect(ui, [this](std::string model_name) {
            trace.push_back(Json::array({"model_download_started", model_name}));
        }));
        links.push_back(r.model_download_progress.connect(
            ui, [this](std::string model_name, std::int64_t done, std::int64_t total) {
                trace.push_back(Json::array({"model_download_progress", model_name, done, total}));
            }));
        links.push_back(
            r.model_download_finished.connect(ui, [this](std::string model_name, bool ok) {
                trace.push_back(Json::array({"model_download_finished", model_name, ok}));
            }));
        links.push_back(r.model_deleted.connect(
            ui, [this](std::string model_name, bool ok, std::string message) {
                trace.push_back(Json::array({"model_deleted", model_name, ok, message}));
            }));
        links.push_back(r.model_cache_changed.connect(
            ui, [this] { trace.push_back(Json::array({"model_cache_changed"})); }));
        links.push_back(
            r.batch_download_planned.connect(ui, [this](std::vector<std::string> models) {
                trace.push_back(Json::array({"batch_download_planned", models}));
            }));
        links.push_back(r.batch_download_finished.connect(ui, [this](int done, int total) {
            trace.push_back(Json::array({"batch_download_finished", done, total}));
        }));
        links.push_back(r.status.connect(ui, [this](std::string text) {
            statuses.push_back(text);
            trace.push_back(Json::array({"status", text}));
        }));
        links.push_back(r.hf_settings_requested.connect(
            ui, [this] { trace.push_back(Json::array({"hf_settings_requested"})); }));
        links.push_back(r.downloads_requested.connect(ui, [this](std::string component) {
            trace.push_back(Json::array({"downloads_requested", component}));
        }));
        auto& c = *components;
        links.push_back(c.component_install_started.connect(ui, [this](std::string component) {
            trace.push_back(Json::array({"component_install_started", component}));
        }));
        links.push_back(c.component_install_finished.connect(
            ui, [this](std::string component, bool ok, std::string message) {
                trace.push_back(
                    Json::array({"component_install_finished", component, ok, message}));
            }));
        links.push_back(c.component_state_changed.connect(
            ui, [this] { trace.push_back(Json::array({"component_state_changed"})); }));
        links.push_back(
            c.component_progress.connect(ui, [this](std::string component, std::string phase,
                                                    std::int64_t done, std::int64_t total) {
                trace.push_back(Json::array({"component_progress", component, phase, done, total}));
            }));
        links.push_back(c.status.connect(ui, [this](std::string text) {
            statuses.push_back(text);
            trace.push_back(Json::array({"status", text}));
        }));
    }

    // Runs `fn` as a UI-executor task, so the signals it emits reach the presenter inline, as
    // OpenWhisper's synchronous slots do.
    template <class F> void on_ui(F fn) {
        ui.post(std::move(fn));
        ui.run_all();
    }

    // Runs the UI executor and the downloads strand until both are idle; the components strand
    // only when asked.
    void settle(bool with_components = false) {
        for (;;) {
            std::size_t ran = ui.run_all() + work.run_all();
            if (with_components) {
                ran += component_strand.run_all();
            }
            if (ran == 0) {
                return;
            }
        }
    }

    // The model's files, downloaded before the test starts.
    void cache_model(const std::string& key) { cache->download(key); }

    void set_policy(std::string_view policy) { save_hf_access_policy(settings, policy); }

    // The trace entries named `name`.
    Json entries(std::string_view name) const {
        Json found = Json::array();
        for (const Json& entry : trace) {
            if (entry.at(0).get<std::string>() == name) {
                found.push_back(entry);
            }
        }
        return found;
    }
    std::size_t count(std::string_view name) const { return entries(name).size(); }
    // The last entry named `name`, or null when there is none.
    Json last_entry(std::string_view name) const {
        const Json found = entries(name);
        return found.empty() ? Json() : found.back();
    }
    // The last status, or "" before any.
    std::string last_status() const { return statuses.empty() ? std::string() : statuses.back(); }
    bool has_status(std::string_view text) const {
        for (const std::string& status : statuses) {
            if (status == text) {
                return true;
            }
        }
        return false;
    }
};

} // namespace fv::models::test
