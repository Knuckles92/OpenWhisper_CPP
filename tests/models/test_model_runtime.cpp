// The model runtime: download consent, downloads, batches, deletes and the runtime question. The
// ModelRuntime tests of OpenWhisper's tests/test_application_controller.py, over a fake engine,
// the fake HTTP client and manual executors, and a trace of the signals for a consent-then-
// download and a batch with a stop.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "model_runtime_harness.h"
#include "models/progress_throttle.h"

using namespace fv::models;
using namespace fv::models::test;
using fv::Json;

namespace {

constexpr std::string_view kEngineRefusal = "End the meeting before changing the engine";

bool installed_json_exists(const RuntimeHarness& h, const std::string& key) {
    return fs::exists(h.temp.path() / "speech-models" / key / "installed.json");
}

} // namespace

// --- downloads into the engine ---

TEST_CASE("TestApplicationController::test_hf_worker_does_not_load_into_the_engine_during_a_"
          "meeting",
          "[test_application_controller]") {
    // from tests/test_application_controller.py: a cached selected model reaches the engine only
    // through reload_engine, which waits for the meeting to end.
    RuntimeHarness h;
    h.build();
    h.cache_model(kParakeet);
    h.engine.set_meeting_active(true);

    h.on_ui([&] { h.runtime->request_download(kParakeet, true); });
    h.settle();

    CHECK(h.entries("model_download_finished") ==
          Json::array({Json::array({"model_download_finished", kParakeet, true})}));
    CHECK(h.engine.reloads() == 0);
    CHECK(h.has_status(kEngineRefusal));
}

TEST_CASE("TestApplicationController::test_cache_fetch_does_not_revive_the_engine_during_a_"
          "meeting",
          "[test_application_controller]") {
    // The download still succeeds; only the engine revival waits.
    RuntimeHarness h;
    h.build();
    h.set_policy("always");
    h.engine.set_meeting_active(true);

    h.on_ui([&] { h.runtime->request_download(kParakeet, false); });
    h.settle();

    CHECK(installed_json_exists(h, kParakeet));
    CHECK(h.entries("model_download_finished") ==
          Json::array({Json::array({"model_download_finished", kParakeet, true})}));
    CHECK(h.engine.reloads() == 0);
    CHECK(h.has_status(kEngineRefusal));
}

TEST_CASE("TestApplicationController::test_hf_worker_loads_into_the_engine_without_a_meeting",
          "[test_application_controller]") {
    RuntimeHarness h;
    h.build();
    h.cache_model(kParakeet);

    h.on_ui([&] { h.runtime->request_download(kParakeet, true); });
    h.settle();

    CHECK(h.engine.reloads() == 1);
    CHECK_FALSE(h.has_status(kEngineRefusal));
}

// --- Downloads' fetch-only downloads ---

TEST_CASE("TestApplicationController::test_manager_download_consent_cancel_keeps_selection",
          "[test_application_controller]") {
    // A declined fetch-only download must not change the model selection.
    RuntimeHarness h;
    h.settings.save_setting("local_asr_models", Json{{"parakeet", kParakeet}});
    h.consent_answer = ConsentAction::Cancel;
    h.build();

    h.on_ui([&] { h.runtime->request_download(kOrukeet, false); });

    CHECK(h.last_entry("hf_consent_requested") ==
          Json::array({"hf_consent_requested", kOrukeet, false, false}));
    CHECK(h.settings.snapshot()->at("local_asr_models") == Json{{"parakeet", kParakeet}});
    CHECK(h.work.queued() == 0);
    CHECK(h.access->requests_in_flight() == 0);
    CHECK(h.has_status("Model 'orukeet-v0.1' is unavailable — download declined"));
}

TEST_CASE("TestApplicationController::test_manager_download_already_cached_short_circuits",
          "[test_application_controller]") {
    RuntimeHarness h;
    h.build();
    h.cache_model(kOrukeet);
    const auto fetched = h.client.requests().size();

    h.on_ui([&] { h.runtime->request_download(kOrukeet, false); });

    CHECK(h.access->requests_in_flight() == 0);
    CHECK(h.count("model_cache_changed") == 1);
    CHECK(h.work.queued() == 0);
    CHECK(h.client.requests().size() == fetched);
}

TEST_CASE("TestApplicationController::test_manager_fetch_only_download_leaves_engine_alone",
          "[test_application_controller]") {
    RuntimeHarness h;
    h.build();
    h.set_policy("always");
    h.engine.set_loaded_model(kParakeet);

    h.on_ui([&] { h.runtime->request_download(kOrukeet, false); });
    CHECK(h.entries("model_download_started") ==
          Json::array({Json::array({"model_download_started", kOrukeet})}));
    CHECK(h.work.queued() == 1);
    h.settle();

    CHECK(h.client.requests().size() == 1);
    CHECK(installed_json_exists(h, kOrukeet));
    CHECK(h.entries("model_download_finished") ==
          Json::array({Json::array({"model_download_finished", kOrukeet, true})}));
    CHECK(h.count("model_cache_changed") == 1);
    // The engine is untouched: no reload, no release.
    CHECK(h.engine.calls().empty());
    CHECK(h.statuses ==
          std::vector<std::string>{"Downloading model 'orukeet-v0.1' from Hugging Face...",
                                   "Model 'orukeet-v0.1' downloaded"});
}

TEST_CASE("TestApplicationController::test_manager_fetch_bridges_missing_selected_model",
          "[test_application_controller]") {
    // Fetching the missing selected model revives the engine.
    RuntimeHarness h;
    h.build();
    h.set_policy("always");

    h.on_ui([&] { h.runtime->request_download(kParakeet, false); });
    h.settle();

    CHECK(installed_json_exists(h, kParakeet));
    CHECK(h.engine.reloads() == 1);
}

TEST_CASE("TestApplicationController::test_optional_download_never_marks_engine_busy",
          "[test_application_controller]") {
    // The selected model's own flow runs the fetch-only worker: nothing touches the engine until
    // the download has finished, and the consent question carries load_into_engine.
    RuntimeHarness h;
    h.consent_answer = ConsentAction::DownloadOnce;
    h.build();

    h.on_ui([&] { h.runtime->request_download(kParakeet, true); });
    CHECK(h.entries("hf_consent_requested") ==
          Json::array({Json::array({"hf_consent_requested", kParakeet, false, true})}));
    CHECK(h.work.queued() == 1);
    CHECK(h.engine.calls().empty());
    h.work.run_all();
    CHECK(h.engine.calls().empty());
    h.settle();
    CHECK(h.engine.calls() == std::vector<std::string>{"reload_engine"});
}

// --- batches ---

TEST_CASE("TestApplicationController::test_batch_download_releases_every_processed_request",
          "[test_application_controller]") {
    RuntimeHarness h;
    h.build();
    const std::vector<std::string> queue{kParakeet, "custom/model", kOrukeet};

    h.on_ui([&] { h.runtime->request_batch(queue); });
    CHECK(h.access->requests_in_flight() == 3);
    h.settle();

    // "custom/model" is no speech model: its download fails and the queue goes on.
    CHECK(h.access->requests_in_flight() == 0);
    for (const auto& name : queue) {
        CHECK(h.access->begin_request(name));
    }
    CHECK(h.entries("batch_download_finished") ==
          Json::array({Json::array({"batch_download_finished", 2, 3})}));
    CHECK(h.entries("model_download_finished") ==
          Json::array({Json::array({"model_download_finished", kParakeet, true}),
                       Json::array({"model_download_finished", "custom/model", false}),
                       Json::array({"model_download_finished", kOrukeet, true})}));
}

TEST_CASE("TestApplicationController::test_stopped_batch_releases_processed_and_unprocessed_"
          "requests",
          "[test_application_controller]") {
    RuntimeHarness h;
    h.cache_options.download_file = [&h](std::string_view, std::string_view, std::int64_t,
                                         const fs::path& destination, const InstallProgress&,
                                         const fv::CancelToken&, std::int64_t, std::int64_t) {
        h.runtime->stop_batch();
        write_file(destination, kWeights);
    };
    h.build();
    const std::vector<std::string> queue{kParakeet, "custom/model", kOrukeet};

    h.on_ui([&] { h.runtime->request_batch(queue); });
    h.settle();

    CHECK(h.access->requests_in_flight() == 0);
    for (const auto& name : queue) {
        CHECK(h.access->begin_request(name));
    }
    CHECK(h.entries("batch_download_finished") ==
          Json::array({Json::array({"batch_download_finished", 1, 3})}));
    CHECK(h.count("model_download_started") == 1);
}

TEST_CASE("a batch is refused offline or beside another request, and a cached one only rescans",
          "[models]") {
    RuntimeHarness h;
    h.build();
    h.env.set("HF_HUB_OFFLINE", "1");
    h.on_ui([&] { h.runtime->request_batch({kParakeet}); });
    CHECK(h.last_status() == "Downloads are disabled by HF_HUB_OFFLINE");
    h.env.unset("HF_HUB_OFFLINE");

    REQUIRE(h.access->begin_request(kOrukeet));
    h.on_ui([&] { h.runtime->request_batch({kParakeet}); });
    CHECK(h.last_status() == "Another model request is in progress — try again when it finishes");
    h.access->end_request(kOrukeet);

    h.cache_model(kParakeet);
    h.on_ui([&] { h.runtime->request_batch({kParakeet}); });
    CHECK(h.count("batch_download_planned") == 0);
    CHECK(h.count("model_cache_changed") == 1);
    CHECK(h.work.queued() == 0);
}

// --- deletes ---

TEST_CASE("TestApplicationController::test_manager_delete_refuses_loaded_model",
          "[test_application_controller]") {
    RuntimeHarness h;
    h.build();
    h.engine.set_loaded_model(kParakeet);

    h.on_ui([&] { h.runtime->request_delete(kParakeet); });

    CHECK(h.entries("model_deleted") ==
          Json::array({Json::array(
              {"model_deleted", kParakeet, false, "Model is in use — switch models first"})}));
    CHECK(h.work.queued() == 0);
}

TEST_CASE("TestApplicationController::test_manager_delete_runs_worker_and_reports",
          "[test_application_controller]") {
    RuntimeHarness h;
    h.build();
    h.cache_model(kOrukeet);

    h.on_ui([&] { h.runtime->request_delete(kOrukeet); });
    REQUIRE(h.work.queued() == 1);
    h.settle();

    CHECK_FALSE(h.cache->is_cached(kOrukeet));
    CHECK_FALSE(fs::exists(h.temp.path() / "speech-models" / kOrukeet));
    CHECK(h.entries("model_deleted") ==
          Json::array({Json::array({"model_deleted", kOrukeet, true, ""})}));
    CHECK(h.count("model_cache_changed") == 1);
    CHECK(h.access->requests_in_flight() == 0);
    CHECK(h.has_status("Model 'orukeet-v0.1' deleted"));
}

TEST_CASE("TestApplicationController::test_manager_delete_reports_locked_files",
          "[test_application_controller]") {
    RuntimeHarness h;
    h.build();
    // A file where the models folder should be: removal fails with an OS error, as a locked file
    // does.
    write_file(h.temp.path() / "speech-models", "not a folder");

    h.on_ui([&] { h.runtime->request_delete(kOrukeet); });
    h.settle();

    CHECK(h.entries("model_deleted") ==
          Json::array({Json::array(
              {"model_deleted", kOrukeet, false, "Files are in use by another process"})}));
    CHECK(h.access->requests_in_flight() == 0);
}

TEST_CASE("a delete is refused during a meeting and while the model downloads", "[models]") {
    RuntimeHarness h;
    h.build();
    h.engine.set_meeting_active(true);
    h.on_ui([&] { h.runtime->request_delete(kOrukeet); });
    h.engine.set_meeting_active(false);
    REQUIRE(h.access->begin_request(kOrukeet));
    h.on_ui([&] { h.runtime->request_delete(kOrukeet); });

    CHECK(h.entries("model_deleted") ==
          Json::array({Json::array({"model_deleted", kOrukeet, false,
                                    "End the meeting before removing speech models"}),
                       Json::array({"model_deleted", kOrukeet, false,
                                    "A download for this model is in progress"})}));
    CHECK(h.work.queued() == 0);
}

// --- the runtime question ---

TEST_CASE("TestApplicationController::test_required_runtime_prompt_install_or_later",
          "[test_application_controller]") {
    const bool accepted = GENERATE(false, true);
    RuntimeHarness h;
    h.runtime_answer = accepted;
    h.build();

    h.on_ui([&] {
        h.runtime->prompt_for_runtime(kParakeet);
        h.runtime->prompt_for_runtime(kParakeet);
    });

    CHECK(h.entries("runtime_consent_requested") ==
          Json::array({Json::array({"runtime_consent_requested", kParakeet, kCpuRuntime})}));
    CHECK(h.count("component_install_started") == (accepted ? 1u : 0u));
    CHECK(h.count("downloads_requested") == (accepted ? 1u : 0u));
}

TEST_CASE("TestApplicationController::test_required_runtime_prompt_skips_inflight_or_offline",
          "[test_application_controller]") {
    const auto [installing, offline] = GENERATE(std::tuple{true, false}, std::tuple{false, true});
    RuntimeHarness h;
    h.build();
    std::optional<fv::CancelSource> claim;
    if (installing) {
        claim = h.coordinator->begin_install(kCpuRuntime);
    }
    if (offline) {
        h.env.set("HF_HUB_OFFLINE", "1");
    }

    h.on_ui([&] { h.runtime->prompt_for_runtime(kParakeet); });

    CHECK(h.count("runtime_consent_requested") == 0);
}

TEST_CASE("TestApplicationController::test_download_prompts_for_runtime_even_when_model_not_"
          "selected",
          "[test_application_controller]") {
    RuntimeHarness h;
    h.runtime_answer = false;
    h.build();
    h.set_policy("always");
    h.engine.set_selected_backend("api");

    h.on_ui([&] { h.runtime->request_download(kParakeet, false); });

    CHECK(h.count("runtime_consent_requested") == 1);
    CHECK(h.count("model_download_started") == 1);
    CHECK(h.settings.snapshot()->find("selected_model") == h.settings.snapshot()->end());
    h.settle();
    // Not the selected engine: nothing is loaded.
    CHECK(h.engine.reloads() == 0);
}

TEST_CASE("TestApplicationController::test_model_runtime_prompt_waits_for_reload_and_cached_"
          "weights",
          "[test_application_controller]") {
    // The reload half: no question while the reload runs, and the runtime is not counted as
    // asked, so the question comes when the reload has settled. Whether a finished reload asks
    // at all (the weights are cached) is the reload machine's decision.
    RuntimeHarness h;
    h.runtime_answer = true;
    h.build();
    h.engine.set_job_active(true);

    h.on_ui([&] { h.runtime->prompt_for_runtime(kParakeet); });
    CHECK(h.count("runtime_consent_requested") == 0);

    h.engine.set_job_active(false);
    h.on_ui([&] { h.runtime->prompt_for_runtime(kParakeet); });
    CHECK(h.entries("runtime_consent_requested") ==
          Json::array({Json::array({"runtime_consent_requested", kParakeet, kCpuRuntime})}));
    CHECK(h.entries("component_install_started") ==
          Json::array({Json::array({"component_install_started", kCpuRuntime})}));
}

TEST_CASE("TestApplicationController::test_first_use_offers_the_gpu_runtime_with_the_cpu_one",
          "[test_application_controller]") {
    const std::optional<std::string> choice =
        GENERATE(std::optional<std::string>(kCudaRuntime), std::optional<std::string>(kCpuRuntime),
                 std::optional<std::string>());
    RuntimeHarness h(gpu("NVIDIA GeForce RTX 2060", 6144, std::pair{7, 5}));
    h.gpu_answer = choice;
    h.build();

    h.on_ui([&] {
        h.runtime->prompt_for_runtime(kParakeet);
        h.runtime->prompt_for_runtime(kParakeet);
    });

    // Asked once per session, whichever the answer; Later asks again at the next start.
    CHECK(h.entries("gpu_runtime_offer_requested") ==
          Json::array({Json::array(
              {"gpu_runtime_offer_requested", kParakeet, kCudaRuntime, kCpuRuntime})}));
    CHECK(h.count("runtime_consent_requested") == 0);
    if (choice) {
        CHECK(h.entries("component_install_started") ==
              Json::array({Json::array({"component_install_started", *choice})}));
        CHECK(h.entries("downloads_requested") ==
              Json::array({Json::array({"downloads_requested", *choice})}));
    } else {
        CHECK(h.count("component_install_started") == 0);
        CHECK(h.count("downloads_requested") == 0);
    }
}

TEST_CASE("TestApplicationController::test_gpu_runtime_offer_waits_for_its_install_in_flight",
          "[test_application_controller]") {
    RuntimeHarness h(gpu("NVIDIA GeForce RTX 2060", 6144, std::pair{7, 5}));
    h.build();
    const auto claim = h.coordinator->begin_install(kCudaRuntime);
    REQUIRE(claim);

    h.on_ui([&] { h.runtime->prompt_for_runtime(kParakeet); });

    CHECK(h.count("gpu_runtime_offer_requested") == 0);
    CHECK(h.count("runtime_consent_requested") == 0);
}

TEST_CASE("forget_runtime_prompts asks again, and a fetch re-arms its runtime's question",
          "[models]") {
    RuntimeHarness h;
    h.runtime_answer = false;
    h.build();
    h.on_ui([&] { h.runtime->prompt_for_runtime(kParakeet); });
    h.on_ui([&] { h.runtime->prompt_for_runtime(kParakeet); });
    CHECK(h.count("runtime_consent_requested") == 1);

    h.on_ui([&] { h.runtime->forget_runtime_prompts(); });
    h.on_ui([&] { h.runtime->prompt_for_runtime(kParakeet); });
    CHECK(h.count("runtime_consent_requested") == 2);

    // request_model_download discards the runtime from the asked set first.
    h.cache_model(kOrukeet);
    h.on_ui([&] { h.runtime->request_download(kOrukeet, false); });
    CHECK(h.last_entry("runtime_consent_requested") ==
          Json::array({"runtime_consent_requested", kOrukeet, kCpuRuntime}));
}

TEST_CASE("a download that asked about its runtime starts once the question is answered",
          "[models]") {
    RuntimeHarness h;
    h.runtime_answer.reset();
    h.build();
    h.set_policy("always");

    h.on_ui([&] { h.runtime->request_download(kParakeet, false); });
    CHECK(h.count("runtime_consent_requested") == 1);
    CHECK(h.count("model_download_started") == 0);
    CHECK(h.work.queued() == 0);

    h.on_ui([&] { h.runtime->answer_runtime(kCpuRuntime, false); });
    CHECK(h.count("model_download_started") == 1);
    CHECK(h.work.queued() == 1);
}

// --- consent ---

TEST_CASE("the consent answers grant once, allow always, open Settings or report the block",
          "[models]") {
    SECTION("Download once grants this model only") {
        RuntimeHarness h;
        h.consent_answer = ConsentAction::DownloadOnce;
        h.build();
        h.on_ui([&] { h.runtime->request_download(kOrukeet, false); });
        h.settle();
        CHECK(installed_json_exists(h, kOrukeet));
        CHECK(load_hf_access_policy(h.settings) == "ask");
    }
    SECTION("Always allow saves the policy") {
        RuntimeHarness h;
        h.consent_answer = ConsentAction::AlwaysAllow;
        h.build();
        h.on_ui([&] { h.runtime->request_download(kOrukeet, false); });
        h.settle();
        CHECK(installed_json_exists(h, kOrukeet));
        CHECK(load_hf_access_policy(h.settings) == "always");
    }
    SECTION("Open Settings releases the request") {
        RuntimeHarness h;
        h.consent_answer = ConsentAction::OpenSettings;
        h.build();
        h.on_ui([&] { h.runtime->request_download(kOrukeet, false); });
        CHECK(h.count("hf_settings_requested") == 1);
        CHECK(h.access->requests_in_flight() == 0);
        CHECK(h.work.queued() == 0);
    }
    SECTION("HF_HUB_OFFLINE blocks whatever the answer") {
        RuntimeHarness h;
        h.consent_answer = ConsentAction::DownloadOnce;
        h.build();
        h.env.set("HF_HUB_OFFLINE", "yes");
        h.on_ui([&] { h.runtime->request_download(kOrukeet, false); });
        CHECK(h.entries("hf_consent_requested") ==
              Json::array({Json::array({"hf_consent_requested", kOrukeet, true, false})}));
        CHECK(h.statuses ==
              std::vector<std::string>{
                  "Model 'orukeet-v0.1' unavailable — downloads disabled by HF_HUB_OFFLINE"});
        CHECK(h.access->requests_in_flight() == 0);
        CHECK(h.work.queued() == 0);
    }
}

TEST_CASE("a second request for a model in flight asks nothing", "[models]") {
    RuntimeHarness h;
    h.build();
    h.on_ui([&] { h.runtime->request_download(kOrukeet, false); });
    h.on_ui([&] { h.runtime->request_download(kOrukeet, false); });
    CHECK(h.count("hf_consent_requested") == 1);
    CHECK(h.access->requests_in_flight() == 1);
    h.on_ui([&] { h.runtime->answer_consent(kOrukeet, ConsentAction::Cancel); });
    CHECK(h.access->requests_in_flight() == 0);
    // A stale answer is ignored.
    h.on_ui([&] { h.runtime->answer_consent(kOrukeet, ConsentAction::DownloadOnce); });
    CHECK(h.work.queued() == 0);
}

TEST_CASE("the policy turning to always fetches the missing selected model", "[models]") {
    RuntimeHarness h;
    h.build();
    h.set_policy("always");
    h.on_ui([&] { h.runtime->on_hf_policy_changed("always"); });
    CHECK(h.count("model_download_started") == 1);
    h.settle();
    CHECK(installed_json_exists(h, kParakeet));
    CHECK(h.engine.reloads() == 1);

    // A loaded engine, or another policy, fetches nothing.
    RuntimeHarness loaded;
    loaded.build();
    loaded.engine.set_loaded_model(kParakeet);
    loaded.set_policy("always");
    loaded.on_ui([&] { loaded.runtime->on_hf_policy_changed("always"); });
    loaded.on_ui([&] { loaded.runtime->on_hf_policy_changed("ask"); });
    CHECK(loaded.count("model_download_started") == 0);
    CHECK(loaded.count("hf_consent_requested") == 0);
}

TEST_CASE("the missing and loaded local model and runtime", "[models]") {
    RuntimeHarness h;
    h.build();
    CHECK(h.runtime->missing_local_runtime() == kCpuRuntime);
    CHECK_FALSE(h.runtime->loaded_local_model());
    h.engine.set_loaded_model(kParakeet);
    CHECK(h.runtime->loaded_local_model() == kParakeet);
    // A chosen GPU runtime build is the one the engine needs.
    h.settings.save_setting("fv_speech_runtime", "cuda");
    CHECK(h.runtime->missing_local_runtime() == kCudaRuntime);
    // An explicit runtime folder stands in for the component, as resolve_parakeet reads it.
    h.settings.save_setting("fv_runtime_dir", "my-runtime");
    CHECK_FALSE(h.runtime->missing_local_runtime());
    h.settings.save_setting("fv_runtime_dir", "");
    CHECK(h.runtime->missing_local_runtime() == kCudaRuntime);
    h.engine.set_selected_backend("api");
    CHECK_FALSE(h.runtime->missing_local_runtime());
}

TEST_CASE("a question nobody listens to is answered Cancel or Later at once", "[models]") {
    RuntimeHarness h;
    h.build();
    h.links.clear(); // no presenter
    std::vector<std::string> statuses;
    const auto status_link = h.runtime->status.connect(
        h.ui, [&statuses](std::string text) { statuses.push_back(std::move(text)); });

    h.on_ui([&] { h.runtime->request_download(kOrukeet, false); });
    CHECK(statuses ==
          std::vector<std::string>{"Model 'orukeet-v0.1' is unavailable — download declined"});
    CHECK(h.access->requests_in_flight() == 0);
    CHECK(h.work.queued() == 0);

    // The runtime question is "Later": the download goes ahead and nothing is installed.
    h.set_policy("always");
    h.on_ui([&] { h.runtime->request_download(kOrukeet, false); });
    CHECK(h.work.queued() == 1);
    h.settle(true);
    CHECK(installed_json_exists(h, kOrukeet));
    CHECK_FALSE(h.coordinator->is_installing(kCpuRuntime));
    CHECK(h.engine.released().empty());
}

TEST_CASE("an answer to a question that is not open is ignored", "[models]") {
    RuntimeHarness h;
    h.runtime_answer.reset();
    h.build();
    h.set_policy("always");

    h.on_ui([&] { h.runtime->request_download(kParakeet, false); });
    REQUIRE(h.count("runtime_consent_requested") == 1);
    // The wrong runtime, or the GPU offer's answer, leaves the question open.
    h.on_ui([&] {
        h.runtime->answer_runtime(kCudaRuntime, true);
        h.runtime->answer_gpu_offer(kParakeet, kCpuRuntime);
    });
    CHECK(h.count("model_download_started") == 0);
    CHECK(h.count("component_install_started") == 0);

    // A second download waits behind the open question.
    h.on_ui([&] { h.runtime->request_download(kOrukeet, false); });
    CHECK(h.count("runtime_consent_requested") == 1);
    CHECK(h.count("model_download_started") == 0);

    h.on_ui([&] { h.runtime->answer_runtime(kCpuRuntime, true); });
    CHECK(h.entries("model_download_started") ==
          Json::array({Json::array({"model_download_started", kParakeet}),
                       Json::array({"model_download_started", kOrukeet})}));
    CHECK(h.entries("component_install_started") ==
          Json::array({Json::array({"component_install_started", kCpuRuntime})}));
    // Answered: a repeat answer does nothing.
    h.on_ui([&] { h.runtime->answer_runtime(kCpuRuntime, true); });
    CHECK(h.count("component_install_started") == 1);
}

TEST_CASE("a delete checks the engine again on the downloads strand", "[models]") {
    RuntimeHarness h;
    h.build();
    h.cache_model(kParakeet);

    SECTION("a meeting started since the request") {
        h.on_ui([&] { h.runtime->request_delete(kParakeet); });
        h.engine.set_meeting_active(true);
    }
    SECTION("the model was loaded since the request") {
        h.on_ui([&] { h.runtime->request_delete(kParakeet); });
        h.engine.set_loaded_model(kParakeet);
    }
    h.settle();

    CHECK(h.cache->is_cached(kParakeet));
    CHECK(h.entries("model_deleted") ==
          Json::array({Json::array(
              {"model_deleted", kParakeet, false, "Model is in use — switch models first"})}));
    CHECK(h.count("model_cache_changed") == 0);
    CHECK(h.access->requests_in_flight() == 0);
}

// --- shutdown ---

TEST_CASE("test_cancel_during_runtime_check_does_not_start_worker", "[test_local_asr]") {
    // from tests/test_local_asr.py, adapted: a shutdown that lands while the download worker
    // checks access starts no transfer.
    RuntimeHarness h;
    int checks = 0;
    h.on_cache_check = [&](std::string_view) {
        if (++checks == 2) {
            h.shutdown.cancel();
        }
    };
    h.build();
    h.set_policy("always");

    h.on_ui([&] { h.runtime->request_download(kOrukeet, false); });
    h.settle();

    CHECK(checks == 2);
    CHECK(h.client.requests().empty());
    CHECK_FALSE(installed_json_exists(h, kOrukeet));
    CHECK(h.entries("model_download_finished") ==
          Json::array({Json::array({"model_download_finished", kOrukeet, false})}));
    CHECK(h.access->requests_in_flight() == 0);
}

TEST_CASE("a download cut short by shutdown reports the cancel", "[models]") {
    RuntimeHarness h;
    h.cache_options.download_file = [&h](std::string_view, std::string_view, std::int64_t,
                                         const fs::path&, const InstallProgress&,
                                         const fv::CancelToken&, std::int64_t, std::int64_t) {
        h.shutdown.cancel();
        throw ComponentCanceled();
    };
    h.build();
    h.set_policy("always");

    h.on_ui([&] { h.runtime->request_download(kOrukeet, false); });
    h.settle();

    CHECK(h.entries("model_download_finished") ==
          Json::array({Json::array({"model_download_finished", kOrukeet, false})}));
    CHECK(h.last_status() ==
          describe_model_download_error(kOrukeet, std::make_exception_ptr(ComponentCanceled())));
}

// --- traces ---

TEST_CASE("trace: a missing selected model asks, downloads, then reloads the engine",
          "[models][trace]") {
    RuntimeHarness h;
    h.consent_answer = ConsentAction::DownloadOnce;
    h.runtime_answer = false;
    h.build();

    h.on_ui([&] { h.runtime->ensure_selected_model_available(); });
    h.settle();

    const Json expected = Json::array({
        Json::array({"hf_consent_requested", kParakeet, false, true}),
        Json::array({"runtime_consent_requested", kParakeet, kCpuRuntime}),
        Json::array({"model_download_started", kParakeet}),
        Json::array({"status", "Downloading model 'parakeet-v3' from Hugging Face..."}),
        // The downloader reports the whole file once received and once verified; both are final.
        Json::array({"model_download_progress", kParakeet, kWeights.size(), kWeights.size()}),
        Json::array({"model_download_progress", kParakeet, kWeights.size(), kWeights.size()}),
        Json::array({"status", "Model 'parakeet-v3' downloaded"}),
        Json::array({"model_download_finished", kParakeet, true}),
        Json::array({"reload_engine"}),
        Json::array({"model_cache_changed"}),
    });
    CHECK(h.trace == expected);
    CHECK(installed_json_exists(h, kParakeet));
    CHECK(h.access->requests_in_flight() == 0);

    // Once per runtime per session: the next download of the model asks no more.
    h.on_ui([&] { h.runtime->request_download(kParakeet, true); });
    h.settle();
    CHECK(h.count("runtime_consent_requested") == 1);
    CHECK(h.engine.reloads() == 2);
}

TEST_CASE("trace: a batch stopped during its first model", "[models][trace]") {
    RuntimeHarness h;
    h.cache_options.download_file = [&h](std::string_view, std::string_view, std::int64_t,
                                         const fs::path& destination, const InstallProgress&,
                                         const fv::CancelToken&, std::int64_t, std::int64_t) {
        h.runtime->stop_batch();
        write_file(destination, kWeights);
    };
    h.runtime_answer = false;
    h.build();
    h.engine.set_selected_backend("api");

    h.on_ui([&] { h.runtime->request_batch({kParakeet, kOrukeet}); });
    h.settle();

    const Json expected = Json::array({
        Json::array({"batch_download_planned", Json::array({kParakeet, kOrukeet})}),
        Json::array({"model_download_started", kParakeet}),
        Json::array({"status", "Downloading model 'parakeet-v3' from Hugging Face..."}),
        Json::array({"status", "Model 'parakeet-v3' downloaded"}),
        Json::array({"model_download_finished", kParakeet, true}),
        Json::array({"runtime_consent_requested", kParakeet, kCpuRuntime}),
        Json::array({"model_cache_changed"}),
        Json::array({"batch_download_finished", 1, 2}),
    });
    CHECK(h.trace == expected);
    CHECK(h.access->requests_in_flight() == 0);
}

// --- the progress throttle ---

TEST_CASE("progress passes ten times a second, on a percent change and when final", "[models]") {
    fv::FakeClock clock;
    ProgressThrottle throttle(clock);
    CHECK(throttle.should_emit(0, 1'000'000));
    for (int step = 1; step < 50; ++step) {
        CHECK_FALSE(throttle.should_emit(step * 100, 1'000'000));
    }
    clock.advance(std::chrono::milliseconds(99));
    CHECK_FALSE(throttle.should_emit(6'000, 1'000'000));
    clock.advance(std::chrono::milliseconds(1));
    CHECK(throttle.should_emit(6'100, 1'000'000));
    CHECK(throttle.should_emit(10'000, 1'000'000)); // 1 %
    CHECK_FALSE(throttle.should_emit(10'001, 1'000'000));
    CHECK(throttle.should_emit(1'000'000, 1'000'000)); // final
    CHECK(throttle.should_emit(1'000'000, 1'000'000)); // final again
    // An unknown total is -1 %: only time lets it through.
    ProgressThrottle unknown(clock);
    CHECK(unknown.should_emit(5, 0));
    CHECK_FALSE(unknown.should_emit(10, 0));
    clock.advance(std::chrono::milliseconds(100));
    CHECK(unknown.should_emit(15, 0));

    CHECK(ProgressThrottle::percent(1, 3) == 33);
    CHECK(ProgressThrottle::percent(2, 3) == 66);
    CHECK(ProgressThrottle::percent(5, 0) == -1);
}

TEST_CASE("a large download's progress is throttled at the source", "[models]") {
    RuntimeHarness h;
    h.cache_options.download_file = [](std::string_view, std::string_view, std::int64_t,
                                       const fs::path& destination, const InstallProgress& progress,
                                       const fv::CancelToken&, std::int64_t, std::int64_t) {
        // A 2 GB file in 1 MiB chunks, faster than the clock moves.
        constexpr std::int64_t kTotal = std::int64_t{2048} << 20;
        for (std::int64_t done = 0; done <= kTotal; done += std::int64_t{1} << 20) {
            progress("downloading", done, kTotal);
        }
        write_file(destination, kWeights);
    };
    h.build();
    h.set_policy("always");

    h.on_ui([&] { h.runtime->request_download(kOrukeet, false); });
    h.settle();

    // 0 % to 100 %: one report per whole percent, of 2,049 calls.
    CHECK(h.count("model_download_progress") == 101);
}
