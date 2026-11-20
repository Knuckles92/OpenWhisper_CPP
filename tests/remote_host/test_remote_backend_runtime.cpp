// The client's runtime and model requests to the paired host (transcriber/remote_backend.py
// request_runtime, request_language, refresh_host_catalog and _switch_host's runtime
// precondition; services/runtime/remote.py select_remote_model and select_remote_runtime) over a
// real host on 127.0.0.1 whose speech engine is fake.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <format>
#include <mutex>
#include <stdexcept>

#include "base/identity.h"
#include "base/testing/manual_executor.h"
#include "host_rig.h"
#include "paired_backend.h"
#include "remote/remote_backend.h"
#include "speech/dictation_language.h"

using namespace fv;
using namespace fv::remote_host;
using namespace fv::remote_host::rig;
using fv::remote::RemoteSpeechBackend;
using fv::speech::ReadyState;

namespace {

const Json kModels = Json::array({
    Json{{"family", "parakeet"}, {"model", "parakeet-v3"}, {"label", "Parakeet TDT 0.6B v3"}},
    Json{{"family", "local_whisper"}, {"model", "small"}, {"label", "Whisper small"}},
});

// A host that lists models, switches its fake engine and, with `controls`, takes runtime
// changes (the `switchable` fixture with a host that has engine controls).
struct RuntimeHost {
    explicit RuntimeHost(bool controls = true)
        : paired([this, controls](SpeechHostDeps& deps) {
              deps.models = [] {
                  return kModels;
              };
              deps.select_model = [this](std::string family, std::string model,
                                         std::string device_name, std::optional<std::string>) {
                  {
                      std::lock_guard lock(mutex);
                      switches.push_back(family + "|" + model + "|" + device_name);
                  }
                  if (model == "busy") {
                      throw std::runtime_error(
                          "devbox is transcribing right now. Try again in a moment.");
                  }
                  paired.host.engine->set_family(family);
                  paired.host.engine->set_model(model);
                  return paired.host.engine->describe();
              };
              deps.runtime = [this] {
                  std::lock_guard lock(mutex);
                  return Json{{"family", "parakeet"},
                              {"model", "parakeet-v3"},
                              {"devices", Json::array({"cpu", "cuda"})},
                              {"languages", Json::array({"en", "fr", "auto"})},
                              {"selected", Json{{"device", device}, {"language", language}}}};
              };
              if (controls) {
                  deps.configure_runtime = [this](std::string family, std::string model,
                                                  Json settings, std::string device_name) {
                      std::lock_guard lock(mutex);
                      changes.push_back(family + "|" + model + "|" + settings.dump() + "|" +
                                        device_name);
                      if (settings.contains("device")) {
                          device = settings["device"].get<std::string>();
                      }
                      if (settings.contains("language")) {
                          language = settings["language"].get<std::string>();
                      }
                      return paired.host.engine->describe();
                  };
              }
          }) {}

    std::vector<std::string> switched() {
        std::lock_guard lock(mutex);
        return switches;
    }
    std::vector<std::string> configured() {
        std::lock_guard lock(mutex);
        return changes;
    }

    std::mutex mutex;
    std::vector<std::string> switches;
    std::vector<std::string> changes;
    std::string device = "cuda";
    std::string language = "en";
    PairedBackend paired;
};

} // namespace

TEST_CASE("a runtime change rides on the next load as configure_runtime", "[test_remote_runtime]") {
    RuntimeHost fixture;
    auto& backend = *fixture.paired.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    const auto choices = backend.model_choices();
    REQUIRE(choices.runtime.has_value());
    CHECK((*choices.runtime)["can_configure"] == Json(true));
    CHECK(backend.request_language() == "en");

    backend.request_runtime("parakeet", "parakeet-v3", Json{{"language", "fr"}});
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    // The host is asked even though it already serves the model: the change is the runtime's.
    CHECK(fixture.configured() ==
          std::vector<std::string>{R"(parakeet|parakeet-v3|{"language":"fr"}|laptop)"});
    CHECK(fixture.switched().empty());
    CHECK(backend.request_language() == "fr");
    // It rides on one load only.
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK(fixture.configured().size() == 1);
}

TEST_CASE("a runtime change for a model the host no longer serves is refused",
          "[test_remote_runtime]") {
    RuntimeHost fixture;
    auto& backend = *fixture.paired.backend;
    // Not connected: no model is served at all.
    CHECK_THROWS_WITH(backend.request_runtime("parakeet", "parakeet-v3", Json::object()),
                      "The host changed engines. Reconnect before changing its runtime.");
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK_THROWS_WITH(backend.request_runtime("local_whisper", "small", Json::object()),
                      "The host changed engines. Reconnect before changing its runtime.");
    CHECK(remote::runtime_request_refusal(backend.model_choices(), "local_whisper", "small") ==
          "The host changed engines. Reconnect before changing its runtime.");
    CHECK(remote::runtime_request_refusal(backend.model_choices(), "parakeet", "parakeet-v3")
              .empty());
}

TEST_CASE("a host without engine controls is not asked to change its runtime",
          "[test_remote_runtime]") {
    RuntimeHost fixture(false);
    auto& backend = *fixture.paired.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    const std::string update =
        std::format("Update {} on the host to change its runtime from here.", identity().product);
    CHECK_THROWS_WITH(backend.request_runtime("parakeet", "parakeet-v3", Json::object()), update);
    CHECK(remote::runtime_request_refusal(backend.model_choices(), "parakeet", "parakeet-v3") ==
          update);

    // A request carried over from the backend this one replaced is not checked here; the switch
    // itself says the host can't take it, and the load adopts what the host runs.
    (void)backend.request_model("parakeet", "parakeet-v3", Json{{"device", "cpu"}});
    const auto readiness = backend.load({});
    REQUIRE(readiness.state == ReadyState::Ready);
    CHECK(backend.switch_error() ==
          std::format("Update {} on devbox to change its runtime from here.", identity().product));
    CHECK(fixture.configured().empty());
}

TEST_CASE("choosing a model drops a runtime change requested before it", "[test_remote_runtime]") {
    RuntimeHost fixture;
    auto& backend = *fixture.paired.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    backend.request_runtime("parakeet", "parakeet-v3", Json{{"device", "cpu"}});
    (void)backend.request_model("local_whisper", "small");
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK(fixture.configured().empty());
    CHECK(fixture.switched() == std::vector<std::string>{"local_whisper|small|laptop"});
}

TEST_CASE("the host's catalog refreshes the runtime choices without reconnecting",
          "[test_remote_runtime]") {
    RuntimeHost fixture;
    auto& backend = *fixture.paired.backend;
    const auto pairing = *fixture.paired.pairing;
    const Json catalog{
        {"engine", Json{{"family", "parakeet"}, {"model", "parakeet-v3"}, {"device", "cuda"}}},
        {"runtime", Json{{"family", "parakeet"},
                         {"model", "parakeet-v3"},
                         {"devices", Json::array({"cpu"})},
                         {"selected", Json{{"device", "cpu"}}}}},
        {"can_select", true},
        {"models", Json::array({Json{{"family", "parakeet"},
                                     {"model", "parakeet-v3"},
                                     {"label", "Parakeet TDT 0.6B v3"},
                                     {"cached", true},
                                     {"runtime_ready", true}},
                                Json{{"family", "local_whisper"},
                                     {"model", "small"},
                                     {"label", "Whisper small"},
                                     {"cached", true},
                                     {"runtime_ready", false}}})}};
    const Json ready{{"capabilities", Json{{"engine_controls", true}}}};
    // Nothing is connected yet: nothing to refresh.
    CHECK_FALSE(backend.refresh_host_catalog(pairing, ready, catalog));
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    const auto connections = fixture.paired.connections.load();
    CHECK(backend.refresh_host_catalog(pairing, ready, catalog));
    CHECK(fixture.paired.connections.load() == connections);
    const auto choices = backend.model_choices();
    REQUIRE(choices.runtime.has_value());
    CHECK((*choices.runtime)["devices"] == Json::array({"cpu"}));
    CHECK((*choices.runtime)["can_configure"] == Json(true));
    // Only models downloaded with their runtime ready are offered.
    REQUIRE(choices.models.has_value());
    REQUIRE(choices.models->size() == 1);
    CHECK(choices.models->front().model == "parakeet-v3");

    // An engine change goes through a reconnect, not a refresh.
    Json other = catalog;
    other["engine"]["device"] = "cpu";
    CHECK_FALSE(backend.refresh_host_catalog(pairing, ready, other));
    // Another host's catalog is not this one's.
    auto stranger = pairing;
    stranger.fingerprint = "another";
    CHECK_FALSE(backend.refresh_host_catalog(stranger, ready, catalog));
    // A host that lets nobody choose leaves nothing to choose between.
    Json closed = catalog;
    closed["can_select"] = false;
    CHECK(backend.refresh_host_catalog(pairing, ready, closed));
    CHECK(backend.model_choices().models == std::vector<remote::HostModel>{});
}

// from tests/test_remote_engine.py::test_choosing_a_model_on_the_client_reloads_through_the_host
TEST_CASE("test_choosing_a_model_on_the_client_reloads_through_the_host", "[test_remote_engine]") {
    RuntimeHost fixture;
    auto& backend = *fixture.paired.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    REQUIRE(backend.model_choices().current.has_value());
    CHECK(backend.model_choices().current->key() ==
          std::pair<std::string, std::string>{"parakeet", "parakeet-v3"});
    // The model the host already serves is no switch: no reload.
    CHECK(remote::plan_remote_model_switch(backend.model_choices(), "parakeet", "parakeet-v3",
                                           false, false)
              .skip);

    const auto plan = remote::plan_remote_model_switch(backend.model_choices(), "local_whisper",
                                                       "small", false, false);
    CHECK_FALSE(plan.skip);
    CHECK(plan.refusal.empty());
    CHECK(plan.note == "Switching devbox to Whisper small...");
    (void)backend.request_model("local_whisper", "small");
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK(fixture.switched() == std::vector<std::string>{"local_whisper|small|laptop"});
    CHECK(backend.model_choices().current->key() ==
          std::pair<std::string, std::string>{"local_whisper", "small"});

    // The host says why it kept its model; the reload's message carries it to the status line.
    (void)backend.request_model("parakeet", "busy");
    const auto readiness = backend.load({});
    CHECK(readiness.message == "devbox is transcribing right now. Try again in a moment.");
}

// from tests/test_remote_engine.py::test_the_client_wont_switch_the_host_mid_recording
TEST_CASE("test_the_client_wont_switch_the_host_mid_recording", "[test_remote_engine]") {
    RuntimeHost fixture;
    auto& backend = *fixture.paired.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    const auto choices = backend.model_choices();
    const auto recording =
        remote::plan_remote_model_switch(choices, "nemotron", "nemotron-3.5", false, true);
    CHECK(recording.refusal == "Finish recording before changing the engine");
    CHECK(recording.note.empty());
    CHECK(remote::plan_remote_model_switch(choices, "nemotron", "nemotron-3.5", true, false)
              .refusal == "End the meeting before changing the engine");
    // Refused: nothing was requested, so the next load asks the host for nothing.
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK(fixture.switched().empty());
}

TEST_CASE("a runtime change waits for the current activity", "[test_remote_runtime]") {
    RuntimeHost fixture;
    auto& backend = *fixture.paired.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    const auto choices = backend.model_choices();
    CHECK(remote::plan_remote_runtime_change(choices, "parakeet", "parakeet-v3", true).refusal ==
          "Finish the current activity before changing the remote runtime");
    const auto plan = remote::plan_remote_runtime_change(choices, "parakeet", "parakeet-v3", false);
    CHECK(plan.refusal.empty());
    CHECK(plan.note == "Updating the runtime on devbox...");
    CHECK(remote::plan_remote_runtime_change(choices, "local_whisper", "small", false).refusal ==
          "The host changed engines. Reconnect before changing its runtime.");
}

// from
// tests/test_personalize_s8_language_menu.py::TestControllerWiring::test_remote_engine_reports_reach_the_language_choices
TEST_CASE("test_remote_engine_reports_reach_the_language_choices", "[test_remote_runtime]") {
    // OpenWhisper_CPP: the dictation slot's backend notes the host's languages when it adopts the
    // host (OpenWhisper's controller does it on refresh_remote_models).
    RuntimeHost fixture;
    ManualExecutor retries;
    auto backend = fixture.paired.make(&retries);
    REQUIRE(backend->load({}).state == ReadyState::Ready);
    const auto fingerprint = fixture.paired.pairing->fingerprint;
    struct Reset {
        std::string id;
        ~Reset() { speech::forget_remote_runtime(id); }
    } clean{fingerprint};
    const Json saved{{"selected_model", "remote"},
                     {"remote_engine_client", {{"fingerprint", fingerprint}}},
                     {"dictation_languages", Json::array({"en", "de", "fr"})}};
    // The host takes en, fr and auto: German is not offered.
    CHECK(speech::language_choices(saved) == std::vector<std::string>{"en", "fr"});
    backend.reset();
}
