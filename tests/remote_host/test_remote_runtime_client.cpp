// tests/test_remote_runtime.py's client cases over a real host on 127.0.0.1: the paired
// computer asks for a runtime change (RemoteSpeechBackend::request_runtime), the next load sends
// it as configure_runtime, and the backend adopts what the host then reports. The host's side is
// the `controls` fixture: its runtime is remote_host::runtime_state over a Windows host with both
// Parakeet runtimes and a GPU, and a change is validate_runtime's patch saved to its settings.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <format>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "base/identity.h"
#include "base/testing/manual_executor.h"
#include "host_rig.h"
#include "models/catalog.h"
#include "models/speech_catalog.h"
#include "paired_backend.h"
#include "remote/remote_backend.h"
#include "remote_host/runtime_state.h"
#include "settings/keys.h"

using namespace fv;
using namespace fv::remote_host;
using namespace fv::remote_host::rig;
using Catch::Matchers::ContainsSubstring;
using fv::speech::ReadyState;

namespace {

// tests/test_remote_runtime.py RuntimeEngine: the fake engine, with the device it runs on in its
// identity and description.
class RuntimeEngine final : public HostEngine {
public:
    std::vector<std::string> identity() const override {
        auto out = inner->identity();
        out.push_back(device());
        return out;
    }
    Json describe() const override {
        Json out = inner->describe();
        out["device"] = device();
        out["label"] = "Parakeet TDT 0.6B v3";
        return out;
    }
    Json transcribe(std::span<const float> audio, std::optional<std::string> language,
                    std::span<const std::string> phrases) override {
        return inner->transcribe(audio, std::move(language), phrases);
    }

    std::string device() const {
        std::lock_guard lock(mutex_);
        return device_;
    }
    void set_device(std::string device) {
        std::lock_guard lock(mutex_);
        device_ = std::move(device);
    }

    std::shared_ptr<testing::FakeHostEngine> inner = std::make_shared<testing::FakeHostEngine>();

private:
    mutable std::mutex mutex_;
    std::string device_ = "cpu";
};

models::Host gpu_host() {
    return {
        .platform = std::string(models::platform::kWinAmd64),
        .is_installed =
            [](std::string_view id) {
                return id == models::component_id::kAsrNvidiaCpu ||
                       id == models::component_id::kAsrNvidiaCuda;
            },
        .gpu = [] { return models::NvidiaGpu{"NVIDIA RTX 2060", 6144, {{7, 5}}}; },
    };
}

// The host side of the `controls` fixture: what it reports and how it takes a change.
struct Controls {
    Json runtime() {
        std::lock_guard lock(mutex);
        return runtime_state(engine->describe(), settings, host);
    }
    Json configure(const std::string& family, const std::string& model, const Json& changes) {
        std::lock_guard lock(mutex);
        ++configured;
        if (refusal) {
            throw std::runtime_error(*refusal);
        }
        const auto patch =
            validate_runtime(engine->describe(), settings, host, family, model, changes);
        settings.update(patch);
        auto device = models::selected_device(settings, "parakeet");
        engine->set_device(device == "auto" ? "cuda" : device);
        return engine->describe();
    }
    // The host's deps: serves `engine`, reports runtime(), and takes changes when `take`.
    void install(SpeechHostDeps& deps, bool take) {
        deps.engine_provider = [this] {
            return std::static_pointer_cast<HostEngine>(engine);
        };
        deps.runtime = [this] {
            return runtime();
        };
        if (take) {
            deps.configure_runtime = [this](std::string family, std::string model, Json changes,
                                            std::string) {
                return configure(family, model, changes);
            };
        }
    }
    std::string saved_device() {
        std::lock_guard lock(mutex);
        return models::selected_device(settings, "parakeet");
    }

    std::mutex mutex;
    std::shared_ptr<RuntimeEngine> engine = std::make_shared<RuntimeEngine>();
    Json settings{{std::string(keys::local_asr_devices.key()), Json{{"parakeet", "cpu"}}}};
    models::Host host = gpu_host();
    std::optional<std::string> refusal;
    int configured = 0;
};

struct ControlledHost {
    ControlledHost() : paired([this](SpeechHostDeps& deps) { controls.install(deps, true); }) {}

    // The same host restarted on its port by a version without engine controls (Python's
    // `host._configure_runtime = None`).
    void restart_without_controls() {
        auto& rig = paired.host;
        rig.host->stop();
        rig.host.reset();
        SpeechHostDeps deps{
            .reactor = rig.reactor,
            .registry = rig.registry,
            .identity = test_identity(),
            .host_name = "devbox",
            .clock = system_clock(),
        };
        controls.install(deps, false);
        rig.host = std::make_unique<SpeechHost>(std::move(deps));
        REQUIRE(rig.host->start(rig.port, "127.0.0.1", std::nullopt).has_value());
    }

    Controls controls;
    PairedBackend paired;
};

std::string selected_device(const remote::RemoteSpeechBackend& backend) {
    const auto choices = backend.model_choices();
    REQUIRE(choices.runtime.has_value());
    return (*choices.runtime)["selected"].value("device", "");
}

} // namespace

// tests/test_remote_runtime.py::test_device_change_reloads_same_model_and_adopts_host_precision
TEST_CASE("test_device_change_reloads_same_model_and_adopts_host_precision",
          "[test_remote_runtime]") {
    ControlledHost fixture;
    auto& backend = *fixture.paired.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    REQUIRE(backend.model_choices().runtime.has_value());
    CHECK((*backend.model_choices().runtime)["can_configure"] == Json(true));

    backend.request_runtime("parakeet", "parakeet-v3", Json{{"device", "cuda"}});
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK(backend.is_available());
    CHECK(backend.switch_error().empty());
    CHECK(fixture.controls.engine->device() == "cuda");
    CHECK(backend.link().device == "cuda");
    CHECK_THAT(backend.device_info(), ContainsSubstring("| cuda"));
    CHECK(backend.link().gpu_name == "NVIDIA RTX 2060");
    CHECK(backend.link().gpu_memory_mib == 6144);
    CHECK(selected_device(backend) == "cuda");
    // The host's own engine fields read what was saved there over the wire.
    CHECK(fixture.controls.saved_device() == "cuda");

    // The change rode on one load only.
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK(fixture.controls.configured == 1);

    // FioriVoce's Parakeet has no precision to choose: the host says so and keeps its runtime.
    backend.request_runtime("parakeet", "parakeet-v3", Json{{"compute_type", "int8"}});
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK(backend.switch_error() == "This engine manages its own precision on the host.");
    CHECK(selected_device(backend) == "cuda");
}

// tests/test_remote_runtime.py::test_host_refusal_restores_authoritative_controls
TEST_CASE("test_host_refusal_restores_authoritative_controls", "[test_remote_runtime]") {
    ControlledHost fixture;
    auto& backend = *fixture.paired.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    {
        std::lock_guard lock(fixture.controls.mutex);
        fixture.controls.refusal = "The host is running a meeting.";
    }
    backend.request_runtime("parakeet", "parakeet-v3", Json{{"device", "cuda"}});
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK(backend.is_available());
    CHECK(backend.link().device == "cpu");
    CHECK(backend.switch_error() == "The host is running a meeting.");
    CHECK(selected_device(backend) == "cpu");
}

// tests/test_remote_runtime.py::test_optional_language_is_applied_to_remote_audio: a request made
// without a language carries the one the host's runtime is set to.
TEST_CASE("test_optional_language_is_applied_to_remote_audio", "[test_remote_runtime]") {
    ControlledHost fixture;
    auto& backend = *fixture.paired.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    backend.request_runtime("parakeet", "parakeet-v3", Json{{"language", "fr"}});
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK(backend.request_language() == "fr");
    (void)backend.decode(job_of(tone(1600)));
    auto calls = fixture.controls.engine->inner->calls();
    REQUIRE_FALSE(calls.empty());
    CHECK(calls.back().language == std::optional<std::string>("fr"));

    backend.request_runtime("parakeet", "parakeet-v3", Json{{"language", "auto"}});
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    (void)backend.decode(job_of(tone(1600)));
    calls = fixture.controls.engine->inner->calls();
    CHECK(calls.back().language == std::optional<std::string>("auto"));
}

// tests/test_remote_runtime.py::test_capability_disappearing_before_reload_does_not_send_runtime_changes
TEST_CASE("test_capability_disappearing_before_reload_does_not_send_runtime_changes",
          "[test_remote_runtime]") {
    ControlledHost fixture;
    auto& backend = *fixture.paired.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    backend.request_runtime("parakeet", "parakeet-v3", Json{{"device", "cuda"}});
    fixture.restart_without_controls();
    REQUIRE(wait_for([&] { return backend.check_link() || !backend.is_available(); }));
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK(backend.is_available());
    CHECK_THAT(backend.switch_error(),
               ContainsSubstring(std::format("Update {}", identity().product)));
    CHECK(fixture.controls.configured == 0);
    CHECK(fixture.controls.engine->device() == "cpu");
}

// tests/test_remote_runtime.py::test_idle_reconnect_updates_device_even_when_model_is_unchanged
TEST_CASE("test_idle_reconnect_updates_device_even_when_model_is_unchanged",
          "[test_remote_runtime]") {
    ControlledHost fixture;
    auto& backend = *fixture.paired.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK(backend.link().device == "cpu");
    {
        std::lock_guard lock(fixture.controls.mutex);
        fixture.controls.engine->set_device("cuda");
        fixture.controls.settings.update(
            Json{{std::string(keys::local_asr_devices.key()), Json{{"parakeet", "cuda"}}}});
    }
    (void)fixture.paired.host.host->engine_changed();
    REQUIRE(wait_for([&] { return backend.check_link() || !backend.is_available(); }));
    // The same model: an idle reconnect, not an engine change, and it still adopts the device.
    CHECK(backend.restore_link() == "connected");
    CHECK(backend.link().device == "cuda");
    CHECK_THAT(backend.device_info(), ContainsSubstring("| cuda"));
    CHECK(selected_device(backend) == "cuda");
}

// tests/test_remote_runtime.py::test_old_host_remains_readable_but_cannot_be_configured: a host
// that reports no runtime shows its device and refuses changes.
TEST_CASE("test_old_host_remains_readable_but_cannot_be_configured", "[test_remote_runtime]") {
    PairedBackend paired;
    auto& backend = *paired.backend;
    REQUIRE(backend.load({}).state == ReadyState::Ready);
    CHECK_FALSE(backend.model_choices().runtime.has_value());
    CHECK_THAT(backend.device_info(), ContainsSubstring("| cuda"));
    CHECK_THROWS_WITH(backend.request_runtime("parakeet", "parakeet-v3", Json{{"device", "cuda"}}),
                      ContainsSubstring(std::format("Update {}", identity().product)));
}

// The dictation engine's LinkHandle reads the backend's request timing (the stats line's source,
// transcription.py _timed_decode), and nothing once the backend is gone.
TEST_CASE("the dictation link times the requests of one pass", "[test_remote_runtime]") {
    PairedBackend paired;
    ManualExecutor retries;
    auto backend = paired.make(&retries);
    REQUIRE(backend->load({}).state == ReadyState::Ready);
    const auto handle = backend->link_handle();
    (void)backend->decode(job_of(tone(1600))); // before the mark: not this pass's
    const auto mark = handle.timing_mark();
    REQUIRE(mark.has_value());
    (void)backend->decode(job_of(tone(1600)));
    const auto timing = handle.timing_since(*mark);
    REQUIRE(timing.has_value());
    CHECK(timing->host == "devbox");
    CHECK(timing->requests == 1);
    CHECK(timing->round_trip_s > 0.0);
    REQUIRE(timing->host_s.has_value());
    CHECK(timing->network_s().has_value());
    backend.reset();
    CHECK_FALSE(handle.timing_mark().has_value());
    CHECK_FALSE(handle.timing_since(*mark).has_value());
}
