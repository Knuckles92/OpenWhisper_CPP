// A paired computer switching this host's model (tests/test_remote_engine.py's service and host
// controller tests): HostModelManager over a real SpeechService with a fake engine, a model cache
// in a temporary folder and a FakeEngineControl whose reload the test plays, as the app's reload
// machine would. Each case carries its OpenWhisper function's name.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <condition_variable>
#include <format>
#include <fstream>
#include <future>
#include <mutex>
#include <set>
#include <thread>

#include "../models/models_test_support.h"
#include "base/executor.h"
#include "base/hash.h"
#include "base/testing/manual_executor.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "host_rig.h"
#include "http/testing/fake_http_client.h"
#include "models/component_store.h"
#include "models/coordinator.h"
#include "models/hf_access.h"
#include "models/hf_access_policy.h"
#include "models/speech_catalog.h"
#include "models/speech_model_cache.h"
#include "models/testing/fake_engine_control.h"
#include "models/testing/fake_openwhisper.h"
#include "remote/errors.h"
#include "remote_host/model_management.h"
#include "remote_host/service.h"
#include "settings/keys.h"
#include "speech/testing/fake_backend.h"

using namespace fv;
using namespace fv::remote_host;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string kParakeet = "parakeet-v3";
const std::string kOrukeet = "orukeet-v0.1";

// The host "devbox": Parakeet v3 loaded, model management on, both catalog models downloaded
// with their runtime here unless a test says otherwise.
struct ManagerRig {
    ManagerRig()
        : env{{std::string(identity().env_prefix) + "DATA_DIR", path_to_utf8(root.path())},
              {"LOCALAPPDATA", path_to_utf8(root.path() / "local")},
              {"XDG_DATA_HOME", path_to_utf8(root.path() / "local")},
              {"HOME", path_to_utf8(root.path() / "home")},
              {"USERPROFILE", path_to_utf8(root.path() / "home")}},
          paths(Paths::resolve(identity(), env)), settings(root.path() / "settings.json"),
          store(root.path() / "components", client,
                models::ComponentStoreOptions{.host = models::Host{}}),
          components(store), cache(root.path() / "speech-models", client,
                                   models::SpeechModelCacheOptions{.artifacts = spec_of}),
          access(settings, [this](std::string_view name) { return cache.is_cached(name); }),
          speech(fake->factory(), ui) {
        settings.update(Json{{std::string(keys::remote_host_model_management.key()), true}});
        download(kParakeet);
        download(kOrukeet);
        engine.on_call = [this](const std::string& call) {
            if (call == "reload_engine") {
                std::function<void()> play;
                {
                    std::lock_guard lock(mutex);
                    play = on_reload;
                }
                if (play) {
                    play();
                }
            }
        };
        // What the app's reload does: load the model the settings name, on another thread.
        on_reload = [this] {
            reload_later(50ms);
        };
        load_selected();
        manager = std::make_shared<HostModelManager>(HostModelManagerDeps{
            .paths = paths,
            .settings = settings,
            .cache = cache,
            .components = components,
            .access = access,
            .engine_control = engine,
            .speech = speech,
            .work = ui,
            .ui = ui,
            .shutdown = shutdown.token(),
            .describe = [this] { return describe(); },
            .on_changed = [](std::string) {},
            .host_name = "devbox",
            .resolve = [this](const Json& snapshot) { return resolved(snapshot); },
        });
    }
    ~ManagerRig() {
        shutdown.cancel();
        for (auto& worker : workers) {
            worker.join();
        }
    }

    static Json spec_of(std::string_view key) {
        Json spec = models::testing::small_model_spec("weights.gguf", "weights");
        spec["repo"] = "test/" + std::string(key);
        return spec;
    }
    void download(const std::string& key) {
        models::testing::write_openwhisper_model(root.path(), key, spec_of(key),
                                                 {{"weights.gguf", "weights"}});
        std::lock_guard lock(mutex);
        usable.insert(key);
    }
    // The runtime for `key` is not installed, or its files are not all here.
    void make_unusable(const std::string& key) {
        std::lock_guard lock(mutex);
        usable.erase(key);
    }
    models::ResolvedEngine resolved(const Json& snapshot) {
        models::ResolvedEngine out;
        out.model_key = models::selected_model(snapshot, "parakeet");
        std::lock_guard lock(mutex);
        if (usable.contains(out.model_key)) {
            out.runtime_dir = root.path() / "components" / "asr-nvidia-cpu";
            out.model_path = root.path() / "speech-models" / out.model_key / "weights.gguf";
        } else {
            out.missing_message = "Download it in Downloads.";
        }
        return out;
    }

    // Loads the model the settings select and waits for the load's answer. What describe()
    // says it serves changes before the load, as the app's engine names the model it is loading.
    void load_selected() {
        const auto key = models::selected_model(*settings.snapshot(), "parakeet");
        {
            std::lock_guard lock(mutex);
            served = key;
        }
        speech::BackendConfig config;
        config.backend = "parakeet";
        config.model_key = key;
        const auto before = speech.generation(speech::Slot::Dictation);
        speech.select(speech::Slot::Dictation, config);
        rig::wait_for([&] {
            return speech.generation(speech::Slot::Dictation) > before &&
                   speech.readiness(speech::Slot::Dictation).state != speech::ReadyState::Loading;
        });
    }
    void reload_later(std::chrono::milliseconds delay) {
        std::lock_guard lock(mutex);
        workers.emplace_back([this, delay] {
            std::this_thread::sleep_for(delay);
            load_selected();
        });
    }
    Json describe() {
        const bool available =
            speech.readiness(speech::Slot::Dictation).state == speech::ReadyState::Ready;
        std::lock_guard lock(mutex);
        const auto* model = models::model(served);
        return Json{{"family", "parakeet"},
                    {"model", served},
                    {"label", model ? std::string(model->label) : served},
                    {"device", "cpu"},
                    {"available", available},
                    {"status", available ? "ready" : "not loaded"}};
    }
    std::string saved_model() {
        return keys::local_asr_models.read(*settings.snapshot()).value("parakeet", std::string());
    }
    // select() as a paired computer's request runs it: the reason it gave, or "".
    std::string refusal(const std::string& family, const std::string& model) {
        try {
            (void)manager->select(family, model, std::nullopt);
            return {};
        } catch (const std::exception& error) {
            return error.what();
        }
    }

    testkit::TempDataRoot root;
    MapEnvReader env;
    Paths paths;
    SettingsStore settings;
    http::FakeHttpClient client;
    models::ComponentStore store;
    models::ComponentCoordinator components;
    models::SpeechModelCache cache;
    models::HuggingFaceAccessCoordinator access;
    models::FakeEngineControl engine;
    std::shared_ptr<speech::testing::FakeEngine> fake =
        std::make_shared<speech::testing::FakeEngine>();
    rig::InlineExecutor ui;
    speech::SpeechService speech;
    CancelSource shutdown;

    std::mutex mutex;
    std::set<std::string> usable;
    std::string served;
    std::function<void()> on_reload;
    std::vector<std::thread> workers;
    std::shared_ptr<HostModelManager> manager;
};

std::vector<std::string> keys_of(const std::vector<remote::HostModel>& models) {
    std::vector<std::string> out;
    for (const auto& model : models) {
        out.push_back(model.family + "|" + model.model + "|" + model.label);
    }
    return out;
}

} // namespace

TEST_CASE("test_host_models_are_the_downloaded_ones_with_a_runtime_here") {
    ManagerRig rig;
    const std::vector<std::string> both{"parakeet|parakeet-v3|Parakeet TDT 0.6B v3",
                                        "parakeet|orukeet-v0.1|Orukeet TDT 0.6B"};
    CHECK(keys_of(rig.manager->ready_models()) == both);
    // One whose runtime isn't installed, or that isn't downloaded, would ask something of the
    // person at this computer: not offered.
    rig.make_unusable(kOrukeet);
    CHECK(keys_of(rig.manager->ready_models()) ==
          std::vector<std::string>{"parakeet|parakeet-v3|Parakeet TDT 0.6B v3"});
    // Pairing alone never grants model administration.
    rig.settings.update(Json{{std::string(keys::remote_host_model_management.key()), false}});
    CHECK(rig.manager->ready_models().empty());
}

TEST_CASE("test_service_answers_a_switch_once_the_new_model_has_loaded") {
    ManagerRig rig;
    const Json described = rig.manager->select("parakeet", kOrukeet, std::nullopt);
    CHECK(described.at("family") == "parakeet");
    CHECK(described.at("model") == kOrukeet);
    CHECK(described.at("available") == true);
    CHECK(rig.engine.reloads() == 1);
    CHECK(rig.manager->ready_models().size() == 2);
}

TEST_CASE("test_the_host_switches_as_if_the_model_were_picked_there") {
    ManagerRig rig;
    rig.settings.update(Json{{"selected_model", "api"}});
    (void)rig.manager->select("parakeet", kOrukeet, std::nullopt);
    // Saved the way the engine fields save it, with its backend selected, then one reload.
    CHECK(rig.saved_model() == kOrukeet);
    CHECK(keys::selected_model.read(*rig.settings.snapshot()) == "parakeet");
    CHECK(rig.engine.calls() == std::vector<std::string>{"reload_engine"});
}

TEST_CASE("test_service_refuses_what_isnt_ready_and_skips_what_already_runs") {
    ManagerRig rig;
    rig.make_unusable(kOrukeet);
    CHECK(rig.refusal("parakeet", kOrukeet) ==
          "orukeet-v0.1 isn't ready on devbox. Download it there first.");
    // A model of an engine this build doesn't have is not among the ready ones either.
    CHECK_THAT(rig.refusal("qwen_asr", "qwen-1.7b"), ContainsSubstring("isn't ready on"));
    const Json current = rig.manager->select("parakeet", kParakeet, std::nullopt);
    CHECK(current.at("model") == kParakeet);
    CHECK(rig.engine.reloads() == 0);
    CHECK(rig.saved_model().empty());
}

TEST_CASE("test_service_reports_a_refused_or_failed_switch") {
    ManagerRig rig;
    // The load fails: the paired computer hears the engine's own status.
    {
        std::lock_guard lock(rig.mutex);
        rig.on_reload = [&rig] {
            speech::testing::FakeScript failing;
            failing.readiness = {speech::ReadyState::Failed, "not loaded", "not loaded"};
            rig.fake->set_script(failing);
            rig.reload_later(50ms);
        };
    }
    CHECK_THAT(rig.refusal("parakeet", kOrukeet), ContainsSubstring("not loaded"));
    // This computer can't switch right now: its reason is passed on as it is.
    rig.fake->set_script({});
    rig.load_selected();
    rig.engine.set_meeting_active(true);
    CHECK(rig.refusal("parakeet", kParakeet) ==
          "devbox is running a meeting. Change its model after the meeting ends.");
}

TEST_CASE("test_the_host_refuses_a_switch_while_busy_or_for_a_model_it_cant_run") {
    ManagerRig rig;
    rig.engine.set_meeting_active(true);
    CHECK_THAT(rig.refusal("parakeet", kOrukeet), ContainsSubstring("running a meeting"));
    rig.engine.set_meeting_active(false);
    rig.engine.set_job_active(true);
    CHECK(rig.refusal("parakeet", kOrukeet) ==
          "devbox is transcribing right now. Try again in a moment.");
    rig.engine.set_job_active(false);
    // A model that is not this family's: refused, as one this computer can't run.
    try {
        (void)rig.manager->select("nemotron", kOrukeet, std::string("cpu"));
        FAIL("a model of another family was selected");
    } catch (const std::exception& error) {
        CHECK(std::string_view(error.what()) == "devbox can't run orukeet-v0.1.");
    }
    // Nothing was saved and nothing reloaded.
    CHECK(rig.saved_model().empty());
    CHECK(rig.engine.reloads() == 0);
}

TEST_CASE("test_a_queued_reload_is_unsettled_until_it_starts_and_ends") {
    ManagerRig rig;
    {
        std::lock_guard lock(rig.mutex);
        rig.on_reload = [] {
        }; // queued: the debounce has not fired yet
    }
    auto answer = std::async(std::launch::async, [&] {
        return rig.manager->select("parakeet", kOrukeet, std::nullopt);
    });
    REQUIRE(rig::wait_for([&] { return rig.engine.reloads() == 1; }));
    CHECK(answer.wait_for(300ms) == std::future_status::timeout);
    // The load starts: still unsettled while it runs.
    speech::testing::FakeScript slow;
    slow.load_delay = 400ms;
    rig.fake->set_script(slow);
    rig.reload_later(0ms);
    CHECK(answer.wait_for(200ms) == std::future_status::timeout);
    // And ends: the paired computer has its answer.
    REQUIRE(answer.wait_for(5s) == std::future_status::ready);
    CHECK(answer.get().at("model") == kOrukeet);
}

// RemoteRuntime._switch_engine_to's _reload_note: the host's status line names the model and the
// paired computer that asked while the reload runs; a switch asked here names only the model.
TEST_CASE("the host's reload says which model it switches to and for which computer") {
    ManagerRig rig;
    const std::string label(models::model(kOrukeet)->label);
    (void)rig.manager->select("parakeet", kOrukeet, std::nullopt, "laptop");
    CHECK(rig.engine.last_reload_note() == "Switching to " + label + " for laptop...");
    const std::string back(models::model(kParakeet)->label);
    (void)rig.manager->select("parakeet", kParakeet, std::nullopt);
    CHECK(rig.engine.last_reload_note() == "Switching to " + back + "...");
}

// _apply_engine_overrides: a load running here refuses the switch with the loading sentence.
TEST_CASE("the host refuses a switch while its engine loads") {
    ManagerRig rig;
    speech::testing::FakeScript slow;
    slow.load_delay = 1500ms;
    rig.fake->set_script(slow);
    speech::BackendConfig config;
    config.backend = "parakeet";
    config.model_key = kParakeet;
    rig.speech.select(speech::Slot::Dictation, config);
    REQUIRE(rig::wait_for([&] {
        return rig.speech.readiness(speech::Slot::Dictation).state == speech::ReadyState::Loading;
    }));
    CHECK(rig.refusal("parakeet", kOrukeet) ==
          "devbox is loading its engine. Try again in a moment.");
    CHECK(rig.engine.reloads() == 0);
    CHECK(rig.saved_model().empty());
    rig::wait_for([&] {
        return rig.speech.readiness(speech::Slot::Dictation).state != speech::ReadyState::Loading;
    });
}

// RemoteRuntime.switch_engine_for_client: the UI thread has 10 s to take the switch, else the
// paired computer is told "<host> didn't respond. Try again in a moment.".
TEST_CASE("a host whose UI thread does not take the switch says it did not respond") {
    ManagerRig rig;
    ManualExecutor stuck;
    auto manager = std::make_shared<HostModelManager>(HostModelManagerDeps{
        .paths = rig.paths,
        .settings = rig.settings,
        .cache = rig.cache,
        .components = rig.components,
        .access = rig.access,
        .engine_control = rig.engine,
        .speech = rig.speech,
        .work = rig.ui,
        .ui = stuck,
        .shutdown = rig.shutdown.token(),
        .describe = [&rig] { return rig.describe(); },
        .on_changed = [](std::string) {},
        .host_name = "devbox",
        .resolve = [&rig](const Json& snapshot) { return rig.resolved(snapshot); },
    });
    manager->set_handoff_timeout(200ms);
    try {
        (void)manager->select("parakeet", kOrukeet, std::nullopt, "laptop");
        FAIL("the switch was answered");
    } catch (const remote::RemoteEngineError& error) {
        CHECK(std::string_view(error.what()) == "devbox didn't respond. Try again in a moment.");
    }
    CHECK(rig.engine.reloads() == 0);
}

namespace {

// The rig's manager over another host's components (platform and GPU), for the runtime
// installation cases (tests/test_remote_dependencies.py's `runtimes` fixture).
struct HostRuntimes {
    HostRuntimes(ManagerRig& rig, models::Host host)
        : store(rig.root.path() / "components", rig.client,
                models::ComponentStoreOptions{.host = std::move(host)}),
          components(store),
          manager(std::make_shared<HostModelManager>(HostModelManagerDeps{
              .paths = rig.paths,
              .settings = rig.settings,
              .cache = rig.cache,
              .components = components,
              .access = rig.access,
              .engine_control = rig.engine,
              .speech = rig.speech,
              .work = rig.ui,
              .ui = rig.ui,
              .shutdown = rig.shutdown.token(),
              .describe = [&rig] { return rig.describe(); },
              .on_changed = [](std::string) {},
              .host_name = "devbox",
              .resolve = [&rig](const Json& snapshot) { return rig.resolved(snapshot); },
          })) {
        // The CPU runtime is installed here: ComponentStore reads its sentinel file.
        const auto cpu = store.component_dir(models::component_id::kAsrNvidiaCpu);
        std::filesystem::create_directories(cpu);
        std::ofstream(cpu / ".installed") << "";
    }

    std::string refusal(const Json& fields) {
        try {
            (void)manager->handle("install_runtime", fields);
            return {};
        } catch (const std::exception& error) {
            return error.what();
        }
    }

    models::ComponentStore store;
    models::ComponentCoordinator components;
    std::shared_ptr<HostModelManager> manager;
};

models::Host windows_host(std::optional<models::NvidiaGpu> gpu) {
    return {.platform = std::string(models::platform::kWinAmd64),
            .is_installed =
                [](std::string_view id) { return id == models::component_id::kAsrNvidiaCpu; },
            .gpu =
                [gpu] {
                    return gpu;
                }};
}

} // namespace

// tests/test_remote_dependencies.py::test_install_accepts_only_bundled_model_device_requests: only
// a catalog model and one of the host's devices; nothing is installed otherwise. (The wire's
// refusal of extra fields such as "url" and "component" is test_malformed_mutations_never_reach_
// downloader in test_speech_host.cpp.)
TEST_CASE("test_install_accepts_only_bundled_model_device_requests") {
    ManagerRig rig;
    HostRuntimes host(rig, windows_host(models::NvidiaGpu{"NVIDIA RTX 2060", 6144, {{7, 5}}}));
    for (const Json& fields :
         {Json::object(),
          Json{{"family", "nemotron"}, {"model", "nemotron-3.5"}, {"device", "cuda"}},
          Json{{"family", "parakeet"}, {"model", "parakeet-v3"}, {"device", ""}},
          Json{{"family", "parakeet"}, {"model", "../../arbitrary"}, {"device", "cuda"}}}) {
        CHECK_FALSE(host.refusal(fields).empty());
    }
    CHECK_FALSE(host.components.is_any_installing());
}

// tests/test_remote_dependencies.py::test_install_rejects_missing_or_older_gpu_and_unsupported_platform
// (the Parakeet part: FioriVoce bundles no other engine and no Vulkan runtime).
TEST_CASE("test_install_rejects_missing_or_older_gpu_and_unsupported_platform") {
    ManagerRig rig;
    const Json cuda{{"family", "parakeet"}, {"model", "parakeet-v3"}, {"device", "cuda"}};
    {
        HostRuntimes host(rig, windows_host(std::nullopt));
        CHECK_THAT(host.refusal(cuda), ContainsSubstring("driver"));
    }
    {
        HostRuntimes host(rig, windows_host(models::NvidiaGpu{"GTX 1050 Ti", 4096, {{6, 1}}}));
        CHECK_THAT(host.refusal(cuda), ContainsSubstring("Turing"));
    }
    {
        HostRuntimes host(rig, models::Host{});
        CHECK_THAT(host.refusal(cuda), ContainsSubstring("platform"));
    }
}

// tests/test_remote_dependencies.py::test_local_install_claim_prevents_duplicate_remote_install
TEST_CASE("test_local_install_claim_prevents_duplicate_remote_install") {
    ManagerRig rig;
    HostRuntimes host(rig, windows_host(models::NvidiaGpu{"NVIDIA RTX 2060", 6144, {{7, 5}}}));
    const std::string cuda(models::component_id::kAsrNvidiaCuda);
    auto claim = host.components.begin_install(cuda, CancelSource{});
    REQUIRE(claim);
    CHECK(
        host.refusal(Json{{"family", "parakeet"}, {"model", "parakeet-v3"}, {"device", "cuda"}}) ==
        "This runtime is already being installed on the host. Refresh when it finishes.");
    host.components.end_install(cuda);
}

// dependencies.py validate_model_device: a device that is not ready says why, in the device's
// own words, before the generic "Install ..." (tests/test_remote_dependencies.py::
// test_select_device_requires_cached_model_ready_runtime_and_permission, the device half).
TEST_CASE("test_select_device_requires_cached_model_ready_runtime_and_permission") {
    ManagerRig rig;
    HostRuntimes host(rig, windows_host(std::nullopt));
    try {
        (void)host.manager->select("parakeet", kParakeet, std::string("cuda"), "laptop");
        FAIL("a device without a runtime was selected");
    } catch (const std::exception& error) {
        CHECK(std::string_view(error.what()) ==
              "No NVIDIA GPU detected on the host. Check its NVIDIA driver, or use CPU.");
    }
    rig.settings.update(Json{{std::string(keys::remote_host_model_management.key()), false}});
    try {
        (void)host.manager->select("parakeet", kParakeet, std::string("cpu"), "laptop");
        FAIL("a switch was allowed with model management off");
    } catch (const std::exception& error) {
        CHECK_THAT(std::string(error.what()), ContainsSubstring("disabled"));
    }
    CHECK(rig.engine.reloads() == 0);
}

// model_management.py download: a model not downloaded here needs the host's consent, said in
// OpenWhisper's words; nothing is downloaded.
TEST_CASE("a host that needs download consent says where to give it") {
    ManagerRig rig;
    std::error_code ignored;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(rig.root.path(), ignored)) {
        if (entry.is_directory() && entry.path().filename() == kOrukeet) {
            std::filesystem::remove_all(entry.path(), ignored);
            break;
        }
    }
    if (rig.cache.is_cached(kOrukeet)) {
        SKIP("the rig's copy of the model could not be removed");
    }
    std::string said;
    try {
        (void)rig.manager->handle("download_model",
                                  Json{{"family", "parakeet"}, {"model", kOrukeet}});
    } catch (const std::exception& error) {
        said = error.what();
    }
    CHECK(said == "The host requires download consent. Choose ‘Always allow downloads’ in "
                  "Settings → Downloads on the host, or download this model there first.");
    CHECK(rig.client.requests().empty());
}

// tests/test_remote_runtime.py::test_host_controller_persists_runtime_and_reloads_even_for_same_model:
// a runtime change of the model already served is saved and reloaded, and refused, with nothing
// saved, while this computer transcribes.
TEST_CASE("test_host_controller_persists_runtime_and_reloads_even_for_same_model") {
    ManagerRig rig;
    HostRuntimes host(rig, windows_host(std::nullopt));
    const auto engine =
        host.manager->configure("parakeet", kParakeet, Json{{"device", "cpu"}}, "laptop");
    CHECK(engine.at("model") == kParakeet);
    CHECK(rig.engine.reloads() == 1);
    CHECK(rig.engine.last_reload_note() ==
          "Switching to " + std::string(models::model(kParakeet)->label) + " for laptop...");
    CHECK(keys::local_asr_devices.read(*rig.settings.snapshot()).value("parakeet", "") == "cpu");

    rig.engine.set_job_active(true);
    try {
        (void)host.manager->configure("parakeet", kParakeet, Json{{"device", "auto"}}, "laptop");
        FAIL("a runtime change was made while transcribing");
    } catch (const std::exception& error) {
        CHECK_THAT(std::string(error.what()), ContainsSubstring("transcribing"));
    }
    CHECK(keys::local_asr_devices.read(*rig.settings.snapshot()).value("parakeet", "") == "cpu");
    CHECK(rig.engine.reloads() == 1);
}

// tests/test_remote_dependencies.py::test_controller_applies_target_device_and_refuses_activity:
// a switch that names a device saves it with the model and reloads; while this computer
// transcribes it is refused and the saved device stays.
TEST_CASE("test_controller_applies_target_device_and_refuses_activity") {
    ManagerRig rig;
    HostRuntimes host(rig, windows_host(std::nullopt));
    (void)host.manager->select("parakeet", kParakeet, std::string("cpu"), "laptop");
    CHECK(keys::local_asr_devices.read(*rig.settings.snapshot()).value("parakeet", "") == "cpu");
    CHECK(rig.engine.reloads() == 1);
    rig.engine.set_job_active(true);
    try {
        (void)host.manager->select("parakeet", kParakeet, std::string("cpu"), "laptop");
        FAIL("a device change was made while transcribing");
    } catch (const std::exception& error) {
        CHECK_THAT(std::string(error.what()), ContainsSubstring("transcribing"));
    }
    CHECK(keys::local_asr_devices.read(*rig.settings.snapshot()).value("parakeet", "") == "cpu");
    CHECK(rig.engine.reloads() == 1);
}

// Removes the rig's downloaded copy of `key` (its folder under the data root).
bool forget_download(ManagerRig& rig, const std::string& key) {
    std::error_code ignored;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(rig.root.path(), ignored)) {
        if (entry.is_directory() && entry.path().filename() == key) {
            std::filesystem::remove_all(entry.path(), ignored);
            break;
        }
    }
    return !rig.cache.is_cached(key);
}

// tests/test_remote_model_management.py::test_only_bundled_family_model_pairs_can_be_downloaded
TEST_CASE("test_only_bundled_family_model_pairs_can_be_downloaded") {
    ManagerRig rig;
    for (const Json& fields :
         {Json{{"family", "nemotron"}, {"model", "parakeet-v3"}},
          Json{{"family", "parakeet"}, {"model", "tiny"}},
          Json{{"family", "parakeet"}, {"model", "../../models"}}, Json::object()}) {
        CHECK_THROWS(rig.manager->handle("download_model", fields));
    }
    CHECK(rig.client.requests().empty());
    CHECK(rig.access.requests_in_flight() == 0);
}

// tests/test_remote_model_management.py::test_catalog_is_safe_and_reports_missing_runtime_separately
TEST_CASE("test_catalog_is_safe_and_reports_missing_runtime_separately") {
    ManagerRig rig;
    const Json result = rig.manager->handle("model_catalog", Json::object());
    REQUIRE(result.at("models").is_array());
    for (const auto& entry : result.at("models")) {
        std::set<std::string> keys;
        for (const auto& item : entry.items()) {
            keys.insert(item.key());
        }
        CHECK(keys == std::set<std::string>{"family", "model", "label", "cached", "runtime_ready",
                                            "download_size", "dependencies", "selected_device"});
        for (const auto& dependency : entry.at("dependencies")) {
            CHECK_FALSE(dependency.contains("url"));
            CHECK_FALSE(dependency.contains("path"));
            CHECK_FALSE(dependency.contains("archives"));
        }
        CHECK(entry.at("cached") == true);
    }
    CHECK(result.at("can_select") == true);
    CHECK(result.at("download") == Json::object());
    CHECK(rig.client.requests().empty());
}

// tests/test_remote_model_management.py::test_cached_download_is_noop_and_local_claim_prevents_remote_work
TEST_CASE("test_cached_download_is_noop_and_local_claim_prevents_remote_work") {
    ManagerRig rig;
    const Json fields{{"family", "parakeet"}, {"model", kParakeet}};
    CHECK(rig.manager->handle("download_model", fields).at("download").at("state") == "complete");
    CHECK(rig.client.requests().empty());
    CHECK(rig.access.requests_in_flight() == 0);
    REQUIRE(rig.access.begin_request(kOrukeet));
    try {
        (void)rig.manager->handle("download_model",
                                  Json{{"family", "parakeet"}, {"model", kOrukeet}});
        FAIL("a model claimed here was downloaded for a paired computer");
    } catch (const std::exception& error) {
        CHECK(std::string_view(error.what()) ==
              "This model is already being downloaded or managed on the host.");
    }
    rig.access.end_request(kOrukeet);
    CHECK(rig.client.requests().empty());
}

// tests/test_remote_model_management.py::test_download_respects_host_policy_and_releases_claim:
// without "Always allow downloads" a model not here is refused and its claim given back.
TEST_CASE("test_download_respects_host_policy_and_releases_claim") {
    ManagerRig rig;
    if (!forget_download(rig, kOrukeet)) {
        SKIP("the rig's copy of the model could not be removed");
    }
    const auto policy = rig.access.policy();
    CHECK_THROWS(
        rig.manager->handle("download_model", Json{{"family", "parakeet"}, {"model", kOrukeet}}));
    CHECK(rig.access.requests_in_flight() == 0);
    CHECK(rig.access.policy() == policy);
    CHECK(rig.client.requests().empty());
}

// tests/test_remote_model_management.py::test_worker_start_failure_releases_claim_and_does_not_stay_busy:
// a download whose job never starts (the work executor drops it, as one does after shutdown)
// fails, gives its claim back and does not stay "downloading".
TEST_CASE("test_worker_start_failure_releases_claim_and_does_not_stay_busy") {
    ManagerRig rig;
    if (!forget_download(rig, kOrukeet)) {
        SKIP("the rig's copy of the model could not be removed");
    }
    rig.access.set_policy(models::hf_access_policy::kAlways);
    auto work = std::make_unique<ManualExecutor>();
    auto manager = std::make_shared<HostModelManager>(HostModelManagerDeps{
        .paths = rig.paths,
        .settings = rig.settings,
        .cache = rig.cache,
        .components = rig.components,
        .access = rig.access,
        .engine_control = rig.engine,
        .speech = rig.speech,
        .work = *work,
        .ui = rig.ui,
        .shutdown = rig.shutdown.token(),
        .describe = [&rig] { return rig.describe(); },
        .on_changed = [](std::string) {},
        .host_name = "devbox",
        .resolve = [&rig](const Json& snapshot) { return rig.resolved(snapshot); },
    });
    const Json fields{{"family", "parakeet"}, {"model", kOrukeet}};
    CHECK(manager->handle("download_model", fields).at("download").at("state") == "downloading");
    CHECK(rig.access.requests_in_flight() == 1);
    work.reset(); // the queued job is dropped unrun
    CHECK(rig.access.requests_in_flight() == 0);
    const Json job = manager->handle("model_catalog", Json::object()).at("download");
    CHECK(job.at("state") == "failed");
    CHECK(job.at("error") == "Couldn't start the download on the host.");
    CHECK(rig.client.requests().empty());
}

// tests/test_remote_runtime.py::test_runtime_reply_waits_for_reload_and_reports_cpu_fallback: the
// answer to a runtime change waits for the reload it queued; another change meanwhile is refused;
// and the answer is the engine the reload produced (here CPU, though the GPU was asked for).
TEST_CASE("test_runtime_reply_waits_for_reload_and_reports_cpu_fallback") {
    ManagerRig rig;
    HostRuntimes host(rig, windows_host(models::NvidiaGpu{"NVIDIA RTX 2060", 6144, {{7, 5}}}));
    const auto cuda = host.store.component_dir(models::component_id::kAsrNvidiaCuda);
    std::filesystem::create_directories(cuda);
    std::ofstream(cuda / ".installed") << "";
    {
        std::lock_guard lock(rig.mutex);
        rig.on_reload = [] {
        }; // queued: nothing loads until the test says so
    }
    auto answer = std::async(std::launch::async, [&] {
        return host.manager->configure("parakeet", kParakeet, Json{{"device", "cuda"}}, "laptop");
    });
    REQUIRE(rig::wait_for([&] { return rig.engine.reloads() == 1; }));
    CHECK(answer.wait_for(200ms) == std::future_status::timeout);
    try {
        (void)host.manager->configure("parakeet", kParakeet, Json{{"device", "cpu"}}, "other");
        FAIL("a second runtime change ran while the first was loading");
    } catch (const std::exception& error) {
        CHECK(std::string_view(error.what()) ==
              "The host is changing its runtime. Try again in a moment.");
    }
    CHECK(keys::local_asr_devices.read(*rig.settings.snapshot()).value("parakeet", "") == "cuda");
    rig.reload_later(0ms);
    REQUIRE(answer.wait_for(5s) == std::future_status::ready);
    // The rig's engine reports CPU after any load: what actually runs wins over the request.
    CHECK(answer.get().at("device") == "cpu");
    CHECK(rig.engine.reloads() == 1);
}

// ---- the host's own download and runtime installation jobs ----
// (tests/test_remote_model_management.py's `downloads` fixture and
// tests/test_remote_dependencies.py's `runtimes` fixture: the model files and the runtime archive
// are served by the rig's FakeHttpClient; a Gate holds a transfer in the middle.)

namespace {

// Holds a transfer between its first and second body pieces until released.
struct Gate {
    void close() {
        std::lock_guard lock(mutex);
        open = false;
    }
    void release() {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        changed.notify_all();
    }
    bool wait_entered() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return entered; });
    }
    // A response serving `content` in two pieces, the first `first` bytes long (half by
    // default), stopping before the second while closed.
    http::ScriptedResponse serve(const http::Request& request, std::string_view content,
                                 std::size_t first = 0) {
        auto response = http::FakeHttpClient::serve(request, content);
        response.chunks = {first == 0 ? content.size() / 2 : first};
        auto pieces = std::make_shared<int>(0);
        response.on_chunk = [this, pieces] {
            if (++*pieces != 2) {
                return;
            }
            std::unique_lock lock(mutex);
            entered = true;
            changed.notify_all();
            changed.wait_for(lock, 5s, [&] { return open; });
        };
        return response;
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool open = true;
    bool entered = false;
};

// The archive of asr-nvidia-cuda for win_amd64 as the rig's catalog pins it. Over 1 MiB: the
// download writes (and reports) whole 1 MiB blocks, as OpenWhisper's reads do.
const std::string kCudaArchive =
    models::test::stored_zip({{"bin/nemo_speech_asr_c.dll", "cuda runtime"},
                              {"bin/weights.bin", std::string((3u << 20) / 2, 'x')}});
constexpr std::size_t kPastFirstBlock = (std::size_t{1} << 20) + 16;
const std::string kCudaUrl = "https://example.invalid/nemo-speech-cuda.zip";

Json cuda_catalog() {
    const Json entry{{"version", "0.2.0"},
                     {"component_api", models::kComponentApi},
                     {"platform", "win_amd64"},
                     {"install_bytes", 1000},
                     {"archives", Json::array({Json{{"name", "nemo-speech-cuda.zip"},
                                                    {"url", kCudaUrl},
                                                    {"sha256", sha256_hex(kCudaArchive)},
                                                    {"size_bytes", kCudaArchive.size()},
                                                    {"extract", "zip"}}})}};
    return Json{{"schema", 2},
                {"components", Json{{std::string(models::component_id::kAsrNvidiaCuda),
                                     Json{{"platforms", Json{{"win_amd64", entry}}}}}}}};
}

// The rig's manager with its jobs on a worker of their own (not inline) and its components on
// a Windows host with a Turing GPU, the CPU runtime installed and `catalog` pinning the CUDA one.
struct Jobs {
    explicit Jobs(ManagerRig& rig)
        : rig(rig), store(rig.root.path() / "jobs-components", rig.client, store_options()),
          components(store, models::ComponentCoordinator::Options{.catalog = &catalog}) {
        const auto cpu = store.component_dir(models::component_id::kAsrNvidiaCpu);
        std::filesystem::create_directories(cpu);
        std::ofstream(cpu / ".installed") << "";
        manager = std::make_shared<HostModelManager>(HostModelManagerDeps{
            .paths = rig.paths,
            .settings = rig.settings,
            .cache = rig.cache,
            .components = components,
            .access = rig.access,
            .engine_control = rig.engine,
            .speech = rig.speech,
            .work = pool,
            .ui = rig.ui,
            .shutdown = rig.shutdown.token(),
            .describe = [&rig] { return rig.describe(); },
            .on_changed =
                [this](std::string) {
                    std::lock_guard lock(mutex);
                    ++finished;
                },
            .host_name = "devbox",
            .resolve = [&rig](const Json& snapshot) { return rig.resolved(snapshot); },
        });
    }
    ~Jobs() { gate.release(); }

    static models::ComponentStoreOptions store_options() {
        models::ComponentStoreOptions options;
        options.host = windows_host(models::NvidiaGpu{"NVIDIA RTX 2060", 6144, {{7, 5}}});
        options.free_bytes = [](const std::filesystem::path&) {
            return std::uint64_t{1} << 40;
        };
        return options;
    }
    // Waits for the job's end (on_changed); false after 5 s.
    bool wait_finished(int count = 1) {
        return rig::wait_for(
            [&] {
                std::lock_guard lock(mutex);
                return finished >= count;
            },
            5s);
    }
    Json catalog_job(const char* which) {
        return manager->handle("model_catalog", Json::object()).at(which);
    }
    void management(bool on) {
        rig.settings.update(Json{{std::string(keys::remote_host_model_management.key()), on}});
    }

    ManagerRig& rig;
    Gate gate;
    Json catalog = cuda_catalog();
    models::ComponentStore store;
    models::ComponentCoordinator components;
    std::mutex mutex;
    int finished = 0;
    ThreadPool pool{"jobs", 1};
    std::shared_ptr<HostModelManager> manager;
};

const Json kCudaInstall{{"family", "parakeet"}, {"model", "parakeet-v3"}, {"device", "cuda"}};

std::string error_of(const std::function<void()>& call) {
    try {
        call();
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

} // namespace

// tests/test_remote_model_management.py::test_download_survives_disconnect_and_is_observable_without_selecting:
// the host's download is its own job, watched through the catalog without switching the engine;
// a repeat joins it, another model waits for it, and it finishes with model management turned
// off meanwhile.
TEST_CASE("test_download_survives_disconnect_and_is_observable_without_selecting") {
    ManagerRig rig;
    if (!forget_download(rig, kOrukeet)) {
        SKIP("the rig's copy of the model could not be removed");
    }
    rig.access.set_policy(models::hf_access_policy::kAlways);
    Jobs jobs(rig);
    jobs.gate.close();
    rig.client.set_handler(
        [&](const http::Request& request) { return jobs.gate.serve(request, "weights"); });
    const Json fields{{"family", "parakeet"}, {"model", kOrukeet}};
    CHECK(jobs.manager->handle("download_model", fields).at("download").at("state") ==
          "downloading");
    REQUIRE(jobs.gate.wait_entered());
    const Json status = jobs.catalog_job("download");
    // (The rig's model file is 7 bytes: the download reports whole 1 MiB blocks, so the
    // progress inside one is the runtime installation case's.)
    CHECK(status.at("state") == "downloading");
    CHECK(rig.describe().at("model") == kParakeet);
    // A repeat (a new connection) joins the work; another model waits for it.
    CHECK(jobs.manager->handle("download_model", fields).at("download") == status);
    CHECK_THAT(error_of([&] {
                   (void)jobs.manager->handle("download_model",
                                              Json{{"family", "parakeet"}, {"model", kParakeet}});
               }),
               ContainsSubstring("Another remote"));
    jobs.management(false);
    jobs.gate.release();
    REQUIRE(jobs.wait_finished());
    CHECK(rig.access.requests_in_flight() == 0);
    jobs.management(true);
    CHECK(jobs.catalog_job("download").at("state") == "complete");
    const Json catalog = jobs.manager->handle("model_catalog", Json::object());
    for (const auto& entry : catalog.at("models")) {
        if (entry.at("model") == kOrukeet) {
            CHECK(entry.at("cached") == true);
        }
    }
    CHECK(rig.describe().at("model") == kParakeet);
    CHECK(rig.engine.reloads() == 0);
    CHECK(rig.client.requests().size() == 1);
    (void)jobs.manager->select("parakeet", kOrukeet, std::nullopt, "laptop");
    CHECK(rig.describe().at("model") == kOrukeet);
}

// tests/test_remote_model_management.py::test_failed_download_is_sanitized_releases_claim_and_can_retry
TEST_CASE("test_failed_download_is_sanitized_releases_claim_and_can_retry") {
    ManagerRig rig;
    if (!forget_download(rig, kOrukeet)) {
        SKIP("the rig's copy of the model could not be removed");
    }
    rig.access.set_policy(models::hf_access_policy::kAlways);
    Jobs jobs(rig);
    rig.client.set_handler([](const http::Request&) {
        http::ScriptedResponse failed;
        failed.error = http::Error{http::ErrorKind::Connect,
                                   "secret-token in https://secret@example.com/private/path"};
        return failed;
    });
    const Json fields{{"family", "parakeet"}, {"model", kOrukeet}};
    (void)jobs.manager->handle("download_model", fields);
    REQUIRE(jobs.wait_finished());
    Json job = jobs.catalog_job("download");
    CHECK(job.at("state") == "failed");
    CHECK(job.at("error") == "Download failed on the host. Check its connection, free disk space "
                             "and logs, then retry.");
    CHECK(job.at("error").get<std::string>().find("secret") == std::string::npos);
    CHECK(rig.access.requests_in_flight() == 0);

    rig.client.set_handler([](const http::Request& request) {
        return http::FakeHttpClient::serve(request, "weights");
    });
    (void)jobs.manager->handle("download_model", fields);
    REQUIRE(jobs.wait_finished(2));
    CHECK(jobs.catalog_job("download").at("state") == "complete");
    CHECK(rig.cache.is_cached(kOrukeet));
}

// tests/test_remote_model_management.py::test_incomplete_download_is_never_reported_as_ready: a
// transfer that ends short leaves the job failed and the model not downloaded.
TEST_CASE("test_incomplete_download_is_never_reported_as_ready") {
    ManagerRig rig;
    if (!forget_download(rig, kOrukeet)) {
        SKIP("the rig's copy of the model could not be removed");
    }
    rig.access.set_policy(models::hf_access_policy::kAlways);
    Jobs jobs(rig);
    rig.client.set_handler(
        [](const http::Request& request) { return http::FakeHttpClient::serve(request, "weig"); });
    (void)jobs.manager->handle("download_model", Json{{"family", "parakeet"}, {"model", kOrukeet}});
    REQUIRE(jobs.wait_finished());
    CHECK(jobs.catalog_job("download").at("state") == "failed");
    CHECK(rig.access.requests_in_flight() == 0);
    CHECK_FALSE(rig.cache.is_cached(kOrukeet));
}

// tests/test_remote_dependencies.py::test_install_survives_disconnect_reports_progress_and_uses_host_archive
TEST_CASE("test_install_survives_disconnect_reports_progress_and_uses_host_archive") {
    ManagerRig rig;
    Jobs jobs(rig);
    jobs.gate.close();
    rig.client.set_handler([&](const http::Request& request) {
        return jobs.gate.serve(request, kCudaArchive, kPastFirstBlock);
    });
    CHECK(jobs.manager->handle("install_runtime", kCudaInstall).at("installation").at("state") ==
          "installing");
    REQUIRE(jobs.gate.wait_entered());
    const Json status = jobs.catalog_job("installation");
    CHECK(status.at("state") == "installing");
    CHECK(status.at("phase") == "downloading");
    CHECK(status.at("done").get<std::int64_t>() == std::int64_t{1} << 20);
    CHECK(status.at("total").get<std::int64_t>() > status.at("done").get<std::int64_t>());
    CHECK(jobs.manager->handle("install_runtime", kCudaInstall).at("installation") == status);
    CHECK_THAT(error_of([&] {
                   (void)jobs.manager->handle(
                       "install_runtime",
                       Json{{"family", "parakeet"}, {"model", kOrukeet}, {"device", "cpu"}});
               }),
               ContainsSubstring("in progress"));
    jobs.management(false);
    jobs.gate.release();
    REQUIRE(jobs.wait_finished());
    CHECK_FALSE(jobs.components.is_any_installing());
    CHECK(rig.engine.reloads() == 0); // installing never switches the engine
    jobs.management(true);
    CHECK(jobs.catalog_job("installation").at("state") == "complete");
    const Json catalog = jobs.manager->handle("model_catalog", Json::object());
    CHECK(catalog.at("models").at(0).at("dependencies").at(1).at("ready") == true);
    // The archive came from the host's own catalog.
    REQUIRE(rig.client.requests().size() == 1);
    CHECK(rig.client.requests().front().url == kCudaUrl);
    CHECK(jobs.manager->handle("install_runtime", kCudaInstall).at("installation").at("state") ==
          "complete");
    CHECK(rig.client.requests().size() == 1);
}

// tests/test_remote_dependencies.py::test_install_failure_releases_claim_and_reports_recovery
TEST_CASE("test_install_failure_releases_claim_and_reports_recovery") {
    ManagerRig rig;
    Jobs jobs(rig);
    const auto install = [&](int finished) {
        (void)jobs.manager->handle("install_runtime", kCudaInstall);
        REQUIRE(jobs.wait_finished(finished));
        CHECK_FALSE(jobs.components.is_any_installing());
        return jobs.catalog_job("installation");
    };
    const std::string failed = "Runtime installation failed on the host. Check its connection, "
                               "free disk space and logs, then retry.";
    SECTION("error") {
        rig.client.set_handler([](const http::Request&) {
            http::ScriptedResponse response;
            response.error = http::Error{http::ErrorKind::Connect,
                                         "secret-token in https://secret@example.com/private/path"};
            return response;
        });
        Json job = install(1);
        CHECK(job.at("state") == "failed");
        CHECK(job.at("error") == failed);
        rig.client.set_handler([](const http::Request& request) {
            return http::FakeHttpClient::serve(request, kCudaArchive);
        });
        job = install(2);
        CHECK(job.at("state") == "complete");
    }
    SECTION("incomplete") {
        // The archive does not hold the runtime's library.
        const std::string empty = models::test::stored_zip({{"bin/other.dll", "x"}});
        jobs.catalog["components"]["asr-nvidia-cuda"]["platforms"]["win_amd64"]["archives"][0]
                    ["sha256"] = sha256_hex(empty);
        jobs.catalog["components"]["asr-nvidia-cuda"]["platforms"]["win_amd64"]["archives"][0]
                    ["size_bytes"] = empty.size();
        rig.client.set_handler([empty](const http::Request& request) {
            return http::FakeHttpClient::serve(request, empty);
        });
        const Json job = install(1);
        CHECK(job.at("state") == "failed");
        CHECK(job.at("error") == failed);
    }
    SECTION("cancel") {
        jobs.gate.close();
        rig.client.set_handler(
            [&](const http::Request& request) { return jobs.gate.serve(request, kCudaArchive); });
        (void)jobs.manager->handle("install_runtime", kCudaInstall);
        REQUIRE(jobs.gate.wait_entered());
        jobs.components.cancel_install(models::component_id::kAsrNvidiaCuda);
        jobs.gate.release();
        REQUIRE(jobs.wait_finished());
        CHECK_FALSE(jobs.components.is_any_installing());
        const Json job = jobs.catalog_job("installation");
        CHECK(job.at("state") == "failed");
        CHECK(job.at("error") == "Runtime installation was canceled on the host. You can retry.");
    }
    SECTION("activation") {
        // The engine runs from another runtime folder: the new one takes a restart.
        rig.engine.set_loaded_runtime_dir(
            jobs.store.component_dir(models::component_id::kAsrNvidiaCpu));
        rig.client.set_handler([](const http::Request& request) {
            return http::FakeHttpClient::serve(request, kCudaArchive);
        });
        const Json job = install(1);
        CHECK(job.at("state") == "restart_required");
        CHECK(job.at("error") == std::format("Installed. Restart {} on the host to enable this "
                                             "runtime.",
                                             identity().product));
    }
}

// tests/test_remote_dependencies.py::test_legacy_host_explains_runtime_upgrade and the older-host
// half of
// tests/test_remote_model_management.py::test_client_explains_disabled_or_older_host_without_mutating:
// the client refuses from the host's `ready` before sending anything.
TEST_CASE("test_legacy_host_explains_runtime_upgrade") {
    const std::string product(identity().product);
    const Json current{{"capabilities", Json{{"model_management", true},
                                             {"runtime_installation", true},
                                             {"engine_controls", true}}}};
    CHECK(remote_model_refusal("install_runtime", kCudaInstall, current, product).empty());
    // A host from before runtime installation: managing models, but not runtimes or devices.
    const Json legacy{{"capabilities", Json{{"model_management", true}}}};
    const std::string upgrade = "Update " + product +
                                " on the host to install runtimes and choose a model's device "
                                "remotely.";
    CHECK(remote_model_refusal("install_runtime", kCudaInstall, legacy, product) == upgrade);
    CHECK(remote_model_refusal(
              "select_model", Json{{"family", "parakeet"}, {"model", kParakeet}, {"device", "cpu"}},
              legacy, product) == upgrade);
    CHECK(remote_model_refusal("download_model", Json{{"family", "parakeet"}, {"model", kOrukeet}},
                               legacy, product)
              .empty());
    CHECK(remote_model_refusal("configure_runtime", Json::object(), legacy, product) ==
          "This host cannot change the speech runtime remotely.");
}

TEST_CASE("test_client_explains_disabled_or_older_host_without_mutating") {
    const std::string product(identity().product);
    const Json fields{{"family", "parakeet"}, {"model", kOrukeet}};
    CHECK(remote_model_refusal("download_model", fields,
                               Json{{"capabilities", Json{{"model_management", false}}}},
                               product) ==
          "Model management is disabled on the host. Enable it in Settings → Remote engine there.");
    // A host too old to say anything about model management.
    CHECK(remote_model_refusal("download_model", fields, Json{{"type", "ready"}}, product) ==
          "Update " + product + " on the host to manage its models remotely.");
}

// tests/test_application_controller.py::test_a_paired_computer_switches_this_one_to_another_model:
// refused while this computer runs a meeting; otherwise saved as though picked here, with the
// backend selected, and the paired computer waits until the queued reload has run, its status
// line naming the model and the computer.
TEST_CASE("test_a_paired_computer_switches_this_one_to_another_model") {
    ManagerRig rig;
    rig.settings.update(Json{{"selected_model", "api"}});
    rig.engine.set_meeting_active(true);
    CHECK(rig.refusal("parakeet", kOrukeet) ==
          "devbox is running a meeting. Change its model after the meeting ends.");
    CHECK(keys::selected_model.read(*rig.settings.snapshot()) == "api");
    CHECK(rig.engine.reloads() == 0);
    rig.engine.set_meeting_active(false);
    {
        std::lock_guard lock(rig.mutex);
        rig.on_reload = [] {
        }; // queued: the debounce has not fired
    }
    auto answer = std::async(std::launch::async, [&] {
        return rig.manager->select("parakeet", kOrukeet, std::nullopt, "laptop");
    });
    REQUIRE(rig::wait_for([&] { return rig.engine.reloads() == 1; }));
    CHECK(keys::selected_model.read(*rig.settings.snapshot()) == "parakeet");
    CHECK(rig.saved_model() == kOrukeet);
    CHECK(rig.engine.last_reload_note() ==
          "Switching to " + std::string(models::model(kOrukeet)->label) + " for laptop...");
    // The reload is queued, so the paired computer keeps waiting.
    CHECK(answer.wait_for(200ms) == std::future_status::timeout);
    rig.reload_later(0ms);
    REQUIRE(answer.wait_for(5s) == std::future_status::ready);
    CHECK(answer.get().at("model") == kOrukeet);
}
