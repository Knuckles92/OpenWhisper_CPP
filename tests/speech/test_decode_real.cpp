// Decodes on the installed NeMo-Speech.cpp runtime and Parakeet model. Skipped unless these
// are set (README.md):
//   OPENWHISPER_CPP_TEST_RUNTIME_DIR  an unpacked NeMo-Speech.cpp release
//   OPENWHISPER_CPP_TEST_MODEL        a Parakeet .gguf
//   OPENWHISPER_CPP_TEST_WAV          a recording with speech in it
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/testing/manual_executor.h"
#include "engine/platform.h"
#include "golden/golden.h"
#include "speech/file_windows.h"
#include "speech/parakeet_backend.h"
#include "speech/service.h"
#include "speech_test_support.h"

using fv::speech::ReadyState;
using fv::speech::Slot;

namespace {

std::optional<fv::speech::BackendConfig> installed() {
    const auto runtime = fv::env_utf8("OPENWHISPER_CPP_TEST_RUNTIME_DIR");
    const auto model = fv::env_utf8("OPENWHISPER_CPP_TEST_MODEL");
    if (!runtime || !model) {
        return std::nullopt;
    }
    // The CPU, as the golden decodes ran: a GPU may round differently.
    auto config = fv::speech::test::parakeet_config("cpu");
    config.runtime_dir = fv::path_from_utf8(*runtime);
    config.model_path = fv::path_from_utf8(*model);
    return config;
}

struct RealService {
    explicit RealService(const fv::speech::BackendConfig& config)
        : service(
              [](const fv::speech::BackendConfig& c) {
                  return std::make_unique<fv::speech::ParakeetBackend>(c);
              },
              notify) {
        service.select(Slot::Dictation, config);
        REQUIRE(fv::speech::test::pump_until(
            notify, [&] { return service.readiness(Slot::Dictation).state != ReadyState::Loading; },
            std::chrono::minutes(2)));
        INFO(service.readiness(Slot::Dictation).device_info);
        REQUIRE(service.readiness(Slot::Dictation).state == ReadyState::Ready);
    }

    fv::ManualExecutor notify;
    fv::speech::SpeechService service;
};

fv::RecognitionOptions english() {
    fv::RecognitionOptions options;
    options.language = "en"; // local_asr_language's default, which the vectors decoded with
    return options;
}

} // namespace

TEST_CASE("decode_file returns the text OpenWhisper decoded for each fixture", "[decode]") {
    const auto config = installed();
    if (!config) {
        SKIP("Set OPENWHISPER_CPP_TEST_RUNTIME_DIR and OPENWHISPER_CPP_TEST_MODEL.");
    }
    RealService real(*config);
    fv::golden::for_each("transcriber.optional_backend", "decode", [&](const fv::golden::Case& c) {
        const auto path = fv::speech::test::fixture(c.arg<std::string>(0));
        const auto result = fv::speech::decode_file(real.service, path, english());
        CHECK(result.transcript.text == c.result<std::string>());
    });
}

TEST_CASE("the test recording decodes on the speech thread as the backend decodes it", "[decode]") {
    const auto config = installed();
    const auto wav = fv::env_utf8("OPENWHISPER_CPP_TEST_WAV");
    if (!config || !wav) {
        SKIP("Set OPENWHISPER_CPP_TEST_RUNTIME_DIR, OPENWHISPER_CPP_TEST_MODEL and "
             "OPENWHISPER_CPP_TEST_WAV.");
    }
    const auto path = fv::path_from_utf8(*wav);
    std::string through_service;
    {
        fv::speech::test::LogCapture log;
        RealService real(*config);
        // CTest runs each test case in its own process, so this load is the process's first.
        CHECK_THAT(log.text(),
                   Catch::Matchers::ContainsSubstring(
                       "INFO transcriber.optional_backend: Loaded the NeMo-Speech.cpp ") &&
                       Catch::Matchers::ContainsSubstring(
                           " runtime from " + fv::path_to_utf8(config->runtime_dir) + "\n"));
        const auto result = fv::speech::decode_file(real.service, path, english());
        through_service = result.transcript.text;
        CHECK_FALSE(result.transcript.segments.empty());
        CHECK(result.decode_s > 0.0);
        real.service.warm_up(Slot::Dictation);
    }
    // LocalSpeechBackend.transcribe's own path: every window on this thread, in one call.
    fv::speech::ParakeetBackend backend(*config);
    backend.reload_model();
    REQUIRE(backend.is_available());
    CHECK_FALSE(through_service.empty());
    CHECK(backend.transcribe(path, "en") == through_service);
    CHECK(backend.warmup());
}
