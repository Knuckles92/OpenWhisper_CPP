// Tests against a real runtime and model. They are skipped unless these are
// set, since neither is in the repository:
//   OPENWHISPER_CPP_TEST_RUNTIME_DIR  an unpacked NeMo-Speech.cpp release
//   OPENWHISPER_CPP_TEST_MODEL        a Parakeet .gguf
//   OPENWHISPER_CPP_TEST_WAV          a recording with speech in it

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <optional>
#include <string>

#include "engine/error.h"
#include "engine/platform.h"
#include "engine/recognizer.h"
#include "engine/wav.h"

using Catch::Matchers::ContainsSubstring;

namespace {

std::optional<fv::RecognizerConfig> test_config() {
    const auto runtime = fv::env_utf8("OPENWHISPER_CPP_TEST_RUNTIME_DIR");
    const auto model = fv::env_utf8("OPENWHISPER_CPP_TEST_MODEL");
    if (!runtime || !model) {
        return std::nullopt;
    }
    fv::RecognizerConfig config;
    config.runtime_dir = fv::path_from_utf8(*runtime);
    config.model_path = fv::path_from_utf8(*model);
    return config;
}

} // namespace

TEST_CASE("a missing model is reported before the runtime is touched") {
    fv::RecognizerConfig config;
    config.runtime_dir = "no/such/runtime";
    config.model_path = "no/such/model.gguf";
    CHECK_THROWS_WITH(fv::Recognizer(config),
                      ContainsSubstring("model.gguf") && ContainsSubstring("does not exist"));
}

TEST_CASE("a recording is transcribed with word timings", "[runtime][decode]") {
    const auto config = test_config();
    const auto wav = fv::env_utf8("OPENWHISPER_CPP_TEST_WAV");
    if (!config || !wav) {
        SKIP("Set OPENWHISPER_CPP_TEST_RUNTIME_DIR, OPENWHISPER_CPP_TEST_MODEL and "
             "OPENWHISPER_CPP_TEST_WAV.");
    }
    const fv::AudioBuffer audio = fv::read_wav(fv::path_from_utf8(*wav));
    fv::Recognizer recognizer(*config);
    CHECK_FALSE(recognizer.runtime_version().empty());

    const fv::Transcript transcript = recognizer.transcribe(audio.samples, audio.sample_rate);
    INFO("transcript: " << transcript.text);
    CHECK_FALSE(transcript.text.empty());
    REQUIRE_FALSE(transcript.words.empty());
    REQUIRE_FALSE(transcript.segments.empty());
    CHECK(transcript.words.front().start >= 0.0);
    CHECK(transcript.words.back().end <= audio.duration() + 1.0);
    for (std::size_t i = 1; i < transcript.words.size(); ++i) {
        CHECK(transcript.words[i].start >= transcript.words[i - 1].start);
    }

    // The same recognizer decodes again: nothing from the first request leaks
    // into the second.
    CHECK(recognizer.transcribe(audio.samples, audio.sample_rate).text == transcript.text);
}

TEST_CASE("silence transcribes to nothing rather than failing", "[runtime][decode]") {
    const auto config = test_config();
    if (!config) {
        SKIP("Set OPENWHISPER_CPP_TEST_RUNTIME_DIR and OPENWHISPER_CPP_TEST_MODEL.");
    }
    fv::Recognizer recognizer(*config);
    const std::vector<float> silence(16000, 0.0f);
    const fv::Transcript transcript = recognizer.transcribe(silence, 16000);
    INFO("transcript: " << transcript.text);
    CHECK(transcript.words.size() <= 2);
}
