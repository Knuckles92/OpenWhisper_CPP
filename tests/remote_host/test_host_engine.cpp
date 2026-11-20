// The engine a host serves (services/remote_asr/engines.py): host_engine_for's refusals, the
// SpeechServiceEngine's identity and decode over speech::FakeBackend, phrases, and the fake.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "base/json.h"
#include "base/testing/manual_executor.h"
#include "remote_host/host_engine.h"
#include "remote_host/testing/fake_host_engine.h"
#include "settings/testing/memory_store.h"
#include "speech/service.h"
#include "speech/testing/fake_backend.h"

using fv::Json;
using fv::remote_host::HostEngine;
using fv::remote_host::SpeechServiceEngineDeps;
using namespace std::chrono_literals;

namespace {

// What the deps report of the loaded engine; tests change it as a reload would.
struct Loaded {
    std::string family = "parakeet";
    std::string model = "parakeet-v3";
    std::string label = "Parakeet TDT 0.6B v3";
    std::string device = "cpu";
    std::string info = "Parakeet TDT 0.6B v3 | cpu";
    bool available = true;
};

SpeechServiceEngineDeps deps_for(fv::speech::SpeechService& speech, fv::SettingsStore& settings,
                                 Loaded& loaded) {
    return SpeechServiceEngineDeps{
        .speech = speech,
        .settings = settings,
        .family = [&loaded] { return loaded.family; },
        .model = [&loaded] { return loaded.model; },
        .label = [&loaded] { return loaded.label; },
        .device = [&loaded] { return loaded.device; },
        .device_info = [&loaded] { return loaded.info; },
        .available = [&loaded] { return loaded.available; },
    };
}

fv::speech::BackendConfig parakeet_config() {
    fv::speech::BackendConfig config;
    config.backend = "parakeet";
    config.model_key = "parakeet-v3";
    config.model_label = "Parakeet TDT 0.6B v3";
    config.requested_device = "cpu";
    config.device = "cpu";
    return config;
}

// One window of a 440 Hz tone at 16 kHz.
std::vector<float> tone(std::size_t samples) {
    std::vector<float> out(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        out[i] = 0.2f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 440.0 *
                                                    static_cast<double>(i) / 16000.0));
    }
    return out;
}

// A backend that records what each decode was given, phrases included (speech::FakeBackend
// records the lane and the language only).
struct Seen {
    std::mutex mutex;
    std::vector<std::vector<std::string>> phrases;
    std::vector<std::string> languages;
};

class RecordingBackend final : public fv::speech::Backend {
public:
    explicit RecordingBackend(std::shared_ptr<Seen> seen) : seen_(std::move(seen)) {}
    std::string_view id() const override { return "parakeet"; }
    fv::speech::Capabilities capabilities() const override { return {}; }
    fv::speech::Readiness load(const fv::CancelToken&) override {
        return {fv::speech::ReadyState::Ready, "Parakeet | cpu", ""};
    }
    fv::speech::DecodeResult decode(const fv::speech::DecodeJob& job) override {
        std::lock_guard lock(seen_->mutex);
        seen_->phrases.push_back(job.phrases);
        seen_->languages.push_back(job.options.language);
        fv::speech::DecodeResult result;
        result.transcript.text = "";
        return result;
    }
    void release() override {}

private:
    std::shared_ptr<Seen> seen_;
};

Json run_off_thread(HostEngine& engine, const std::vector<float>& audio,
                    std::optional<std::string> language, std::vector<std::string> phrases = {}) {
    return std::async(std::launch::async,
                      [&] { return engine.transcribe(audio, std::move(language), phrases); })
        .get();
}

} // namespace

TEST_CASE("host refuses to share remote or cloud engines", "[test_remote_engine]") {
    // from tests/test_remote_engine.py::test_host_refuses_to_share_remote_or_cloud_engines
    fv::ManualExecutor notify;
    auto fake = std::make_shared<fv::speech::FakeEngine>();
    fv::speech::SpeechService speech(fake->factory(), notify);
    fv::MemorySettingsStore settings;
    Loaded loaded;

    const auto remote =
        fv::remote_host::host_engine_for("remote", deps_for(speech, settings, loaded));
    CHECK(remote->describe()["status"] ==
          "The host is itself using a remote engine. Select a local engine there.");
    const auto cloud = fv::remote_host::host_engine_for("api", deps_for(speech, settings, loaded));
    CHECK(cloud->describe()["status"] == "The host's selected engine runs in the cloud and can't "
                                         "be shared. Select a local engine there.");
    const auto none = fv::remote_host::host_engine_for("", deps_for(speech, settings, loaded));
    CHECK(none->describe()["status"] == "No transcription engine is selected on the host");
    CHECK(
        none->identity() ==
        std::vector<std::string>{"unavailable", "No transcription engine is selected on the host"});
    for (const auto& engine : {remote, cloud, none}) {
        CHECK(engine->describe()["available"] == false);
        CHECK(engine->identity().front() == "unavailable");
    }
    const auto off = fv::remote_host::unavailable_engine("x");
    CHECK(off->describe() == Json{{"family", ""},
                                  {"model", ""},
                                  {"label", ""},
                                  {"device", ""},
                                  {"streaming", false},
                                  {"available", false},
                                  {"status", "x"}});
    const std::vector<float> audio(160, 0.0f);
    try {
        off->transcribe(audio, "en", {});
        FAIL("transcribe on an unavailable engine returned");
    } catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()) == "x");
    }
    const auto parakeet =
        fv::remote_host::host_engine_for("parakeet", deps_for(speech, settings, loaded));
    CHECK(parakeet->identity().front() == "worker");
}

TEST_CASE("worker identity counts the device", "[test_remote_engine_push]") {
    // from tests/test_remote_engine_push.py::test_worker_identity_counts_the_device
    fv::ManualExecutor notify;
    auto fake = std::make_shared<fv::speech::FakeEngine>();
    fv::speech::SpeechService speech(fake->factory(), notify);
    fv::MemorySettingsStore settings;
    Loaded loaded;
    const auto engine = fv::remote_host::speech_service_engine(deps_for(speech, settings, loaded));
    const auto on_cpu = engine->identity();

    loaded.device = "cuda";

    CHECK(engine->identity() != on_cpu);
    CHECK(engine->identity() ==
          std::vector<std::string>{"worker", "parakeet", "parakeet-v3", "cuda", "auto", "en"});
}

TEST_CASE("saved auto change invalidates idle clients even if resolved runtime is same",
          "[test_remote_runtime]") {
    // from
    // tests/test_remote_runtime.py::test_saved_auto_change_invalidates_idle_clients_even_if_resolved_runtime_is_same
    // (its Whisper half is dropped with Whisper)
    fv::ManualExecutor notify;
    auto fake = std::make_shared<fv::speech::FakeEngine>();
    fv::speech::SpeechService speech(fake->factory(), notify);
    fv::MemorySettingsStore settings;
    Loaded loaded;
    loaded.device = "cuda";
    const auto engine = fv::remote_host::speech_service_engine(deps_for(speech, settings, loaded));

    auto before = engine->identity();
    // The saved device moves from "auto" to "cuda"; the engine runs on CUDA either way.
    settings.save_setting("local_asr_devices", Json{{"parakeet", "cuda"}});
    CHECK(engine->identity() != before);
    CHECK(engine->identity()[4] == "cuda");

    before = engine->identity();
    settings.save_setting("local_asr_language", "auto");
    CHECK(engine->identity() != before);
    CHECK(engine->identity()[5] == "auto");
}

TEST_CASE("host engines declare whether they use hints", "[test_personalize_s4_remote]") {
    // from tests/test_personalize_s4_remote.py::test_host_engines_declare_whether_they_use_hints
    // (Whisper and Nemotron are not ported; Parakeet's dictionary support is "after")
    fv::ManualExecutor notify;
    auto fake = std::make_shared<fv::speech::FakeEngine>();
    fv::speech::SpeechService speech(fake->factory(), notify);
    fv::MemorySettingsStore settings;
    Loaded loaded;
    CHECK_FALSE(fv::remote_host::unavailable_engine("off")->accepts_phrases());
    CHECK_FALSE(fv::remote_host::speech_service_engine(deps_for(speech, settings, loaded))
                    ->accepts_phrases());
}

TEST_CASE("speech worker engine passes phrases to recognize", "[test_personalize_s4_remote]") {
    // from
    // tests/test_personalize_s4_remote.py::test_speech_worker_engine_passes_phrases_to_recognize
    // Adapted: Parakeet takes no phrases, so the language reaches the decode and the phrases
    // never do (test_personalize_s4_recognition's rule, below).
    fv::ManualExecutor notify;
    auto seen = std::make_shared<Seen>();
    fv::speech::SpeechService speech(
        [seen](const fv::speech::BackendConfig&) -> std::unique_ptr<fv::speech::Backend> {
            return std::make_unique<RecordingBackend>(seen);
        },
        notify);
    speech.select(fv::speech::Slot::Dictation, parakeet_config());
    fv::MemorySettingsStore settings;
    Loaded loaded;
    const auto engine = fv::remote_host::speech_service_engine(deps_for(speech, settings, loaded));

    run_off_thread(*engine, std::vector<float>(4, 0.0f), "en");
    run_off_thread(*engine, std::vector<float>(4, 0.0f), "en", {"Ksenia"});
    std::lock_guard lock(seen->mutex);
    CHECK(seen->languages == std::vector<std::string>{"en", "en"});
    CHECK(seen->phrases == std::vector<std::vector<std::string>>{{}, {}});
}

TEST_CASE("a host drops phrases its engine does not boost", "[test_personalize_s4_recognition]") {
    // from
    // tests/test_personalize_s4_recognition.py::TestLocalSpeech::test_a_host_drops_phrases_its_engine_does_not_boost
    fv::ManualExecutor notify;
    auto seen = std::make_shared<Seen>();
    fv::speech::SpeechService speech(
        [seen](const fv::speech::BackendConfig&) -> std::unique_ptr<fv::speech::Backend> {
            return std::make_unique<RecordingBackend>(seen);
        },
        notify);
    speech.select(fv::speech::Slot::Dictation, parakeet_config());
    fv::MemorySettingsStore settings;
    Loaded loaded;
    const auto engine = fv::remote_host::speech_service_engine(deps_for(speech, settings, loaded));

    const std::vector<std::string> phrases{"Ksenia", "Kubernetes"};
    run_off_thread(*engine, std::vector<float>(160, 0.1f), "en", phrases);
    std::lock_guard lock(seen->mutex);
    REQUIRE(seen->phrases.size() == 1);
    CHECK(seen->phrases[0].empty());
}

TEST_CASE("a speech service engine decodes a tone on the RemoteClient lane", "[host_engine]") {
    fv::ManualExecutor notify;
    auto fake = std::make_shared<fv::speech::FakeEngine>();
    fv::speech::FakeScript script;
    script.texts = {"a tone"};
    fake->set_script(script);
    fv::speech::SpeechService speech(fake->factory(), notify);
    speech.select(fv::speech::Slot::Dictation, parakeet_config());
    fv::MemorySettingsStore settings;
    Loaded loaded;
    const auto engine =
        fv::remote_host::host_engine_for("parakeet", deps_for(speech, settings, loaded));

    const Json result = run_off_thread(*engine, tone(16000), "de");
    CHECK(result == Json{{"text", "a tone"},
                         {"segments",
                          Json::array({Json{{"text", "a tone"}, {"start", 0.0}, {"end", 1.0}}})}});
    // No language: the engine's own setting decides, which the job says with "".
    run_off_thread(*engine, tone(8000), std::nullopt);

    std::vector<fv::speech::FakeCall> decodes;
    for (const auto& call : fake->calls()) {
        if (call.op == "decode") {
            decodes.push_back(call);
        }
    }
    REQUIRE(decodes.size() == 2);
    CHECK(decodes[0].lane == fv::speech::Lane::RemoteClient);
    CHECK(decodes[0].samples == 16000);
    CHECK(decodes[0].language == "de");
    CHECK(decodes[1].lane == fv::speech::Lane::RemoteClient);
    CHECK(decodes[1].language.empty());
}

TEST_CASE("a speech service engine passes the engine's failure on", "[host_engine]") {
    fv::ManualExecutor notify;
    fv::speech::FakeScript script;
    script.decode_error = "Parakeet failed to decode.";
    auto fake = std::make_shared<fv::speech::FakeEngine>(script);
    fv::speech::SpeechService speech(fake->factory(), notify);
    speech.select(fv::speech::Slot::Dictation, parakeet_config());
    fv::MemorySettingsStore settings;
    Loaded loaded;
    const auto engine = fv::remote_host::speech_service_engine(deps_for(speech, settings, loaded));
    CHECK_THROWS_WITH(run_off_thread(*engine, tone(1600), "en"), "Parakeet failed to decode.");
}

TEST_CASE("a speech service engine describes the loaded engine", "[host_engine]") {
    fv::ManualExecutor notify;
    auto fake = std::make_shared<fv::speech::FakeEngine>();
    fv::speech::SpeechService speech(fake->factory(), notify);
    fv::MemorySettingsStore settings;
    Loaded loaded;
    loaded.available = false;
    loaded.info = "Download Parakeet TDT 0.6B v3 in Downloads.";
    const auto engine = fv::remote_host::speech_service_engine(deps_for(speech, settings, loaded));
    const Json described = engine->describe();
    std::vector<std::string> keys;
    for (const auto& item : described.items()) {
        keys.push_back(item.key());
    }
    CHECK(keys == std::vector<std::string>{"family", "model", "label", "device", "streaming",
                                           "available", "status"});
    CHECK(described == Json{{"family", "parakeet"},
                            {"model", "parakeet-v3"},
                            {"label", "Parakeet TDT 0.6B v3"},
                            {"device", "cpu"},
                            {"streaming", false},
                            {"available", false},
                            {"status", "Download Parakeet TDT 0.6B v3 in Downloads."}});
    // Parakeet has no live stream on the host.
    CHECK_THROWS_WITH(engine->stream("s", {}, std::nullopt, false),
                      "The host's engine has no live stream");
    engine->cancel_stream("s");
}

TEST_CASE("the fake engine answers as OpenWhisper's FakeEngine", "[host_engine]") {
    fv::remote_host::testing::FakeHostEngine engine;
    CHECK(engine.identity() == std::vector<std::string>{"fake", "parakeet", "parakeet-v3"});
    const std::vector<float> audio(1600, 0.0f);
    const std::vector<std::string> phrases{"Ksenia"};
    const Json result = engine.transcribe(audio, "en", phrases);
    CHECK(result ==
          Json{{"text", "heard 1600"},
               {"segments",
                Json::array({Json{{"text", "heard 1600"}, {"start", 0.0}, {"end", 0.1}}})}});
    CHECK(engine.stream("s1", audio, std::nullopt, true) ==
          Json{{"events", Json::array({Json{{"text", "partial 1600"}, {"final", true}}})}});
    engine.cancel_stream("s1");
    const auto calls = engine.calls();
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].op == "transcribe");
    CHECK(calls[0].language == "en");
    CHECK(calls[0].phrases == phrases);
    CHECK(calls[1].session == "s1");
    CHECK(calls[1].finish);
    CHECK(engine.canceled() == std::vector<std::string>{"s1"});
    CHECK(engine.describe()["status"] == "ready");
    engine.set_available(false);
    CHECK(engine.describe()["status"] == "not loaded");
    engine.set_model("parakeet-v2");
    CHECK(engine.identity()[2] == "parakeet-v2");
    engine.set_failure("the engine fell over");
    CHECK_THROWS_WITH(engine.transcribe(audio, std::nullopt, {}), "the engine fell over");
}
