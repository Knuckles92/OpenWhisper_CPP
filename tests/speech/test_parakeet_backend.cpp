// transcriber/optional_backend.py LocalSpeechBackend for Parakeet: speech::ParakeetBackend over a
// fake decoder, against tests/test_local_asr.py, tests/test_local_transcription_contracts.py,
// tests/test_local_asr_languages.py and the golden vectors of transcriber.optional_backend and
// services.local_asr.nvidia.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "engine/error.h"
#include "engine/transcript.h"
#include "golden/golden.h"
#include "speech/messages.h"
#include "speech/parakeet_backend.h"
#include "speech/testing/fake_decoder.h"
#include "speech/window_decode.h"
#include "speech_test_support.h"

using Catch::Matchers::ContainsSubstring;
using fv::Json;
using fv::speech::ParakeetBackend;
using fv::speech::ReadyState;
using fv::speech::test::parakeet_config;
using fv::speech::testing::FakeDecoders;

namespace {

using namespace std::chrono_literals;

std::shared_ptr<FakeDecoders> decoders(FakeDecoders::Script script = {}) {
    return std::make_shared<FakeDecoders>(std::move(script));
}

FakeDecoders::Script texts(std::vector<std::string> values) {
    FakeDecoders::Script script;
    script.texts = std::move(values);
    return script;
}

FakeDecoders::Script holding_loads() {
    FakeDecoders::Script script;
    script.hold_loads = true;
    return script;
}

FakeDecoders::Script holding_decodes() {
    FakeDecoders::Script script;
    script.hold_decodes = true;
    return script;
}

FakeDecoders::Script gpu_failing(std::string error) {
    FakeDecoders::Script script;
    script.gpu_create_error = std::move(error);
    return script;
}

std::vector<float> signal(const std::string& kind, std::size_t length, double value) {
    std::vector<float> audio(length, 0.0f);
    const auto level = static_cast<float>(value);
    for (std::size_t i = 0; i < length; ++i) {
        if (kind == "constant") {
            audio[i] = level;
        } else if (kind == "ramp") {
            audio[i] =
                static_cast<float>(static_cast<std::int64_t>(i % 3200) - 1600) * level / 1600.0f;
        } else if (kind == "burst") {
            audio[i] = (i / 1600) % 7 == 0 ? level : 0.0f;
        }
    }
    return audio;
}

Json segments_json(const std::vector<fv::Segment>& segments) {
    Json list = Json::array();
    for (const fv::Segment& segment : segments) {
        list.push_back(
            Json{{"text", segment.text}, {"start", segment.start}, {"end", segment.end}});
    }
    return list;
}

} // namespace

TEST_CASE("golden segments_from_words", "[golden]") {
    fv::golden::for_each("services.local_asr.nvidia", "segments_from_words",
                         [](const fv::golden::Case& c) {
                             const Json data = c.arg<Json>(0);
                             std::vector<fv::Word> words;
                             for (const Json& word : data.at("words")) {
                                 words.push_back(fv::Word{word.at("text").get<std::string>(),
                                                          word.at("start").get<double>(),
                                                          word.at("end").get<double>()});
                             }
                             const auto segments = fv::segments_from_words(
                                 data.at("text").get<std::string>(), words, c.arg<double>(1));
                             CHECK(fv::golden::equal(c.result<Json>(), segments_json(segments)));
                         });
}

TEST_CASE("golden join_texts and _audible", "[golden]") {
    fv::golden::for_each("transcriber.optional_backend", "join_texts",
                         [](const fv::golden::Case& c) {
                             CHECK(fv::speech::join_texts(c.arg<std::vector<std::string>>(0)) ==
                                   c.result<std::string>());
                         });
    fv::golden::for_each("transcriber.optional_backend", "_audible", [](const fv::golden::Case& c) {
        const auto values = c.arg<std::vector<double>>(0);
        std::vector<float> audio;
        for (const double value : values) {
            audio.push_back(static_cast<float>(value));
        }
        CHECK(fv::speech::audible(audio) == c.result<bool>());
    });
}

TEST_CASE("golden _transcribe_audio", "[golden]") {
    fv::golden::for_each(
        "transcriber.optional_backend", "_transcribe_audio", [](const fv::golden::Case& c) {
            const auto audio =
                signal(c.arg<std::string>(0), c.arg<std::size_t>(1), c.arg<double>(2));
            std::vector<std::size_t> windows;
            const fv::Transcript result = fv::speech::transcribe_audio(
                audio,
                [&](std::span<const float> window) {
                    windows.push_back(window.size());
                    fv::Transcript piece;
                    piece.text = "w" + std::to_string(windows.size());
                    piece.segments.push_back(
                        fv::Segment{piece.text, 0.0, static_cast<double>(window.size()) / 16000});
                    return piece;
                },
                [] {});
            const Json expected = c.result<Json>();
            CHECK(result.text == expected.at("text").get<std::string>());
            CHECK(fv::golden::equal(expected.at("segments"), segments_json(result.segments)));
            CHECK(windows == expected.at("windows").get<std::vector<std::size_t>>());
        });
}

// from tests/test_local_asr.py::test_chunk_results_keep_offsets_and_silence_is_empty
TEST_CASE("chunk results keep offsets and silence is empty", "[test_local_asr]") {
    auto fake = decoders(texts({"one"}));
    ParakeetBackend backend(parakeet_config(), fake->factory());
    backend.reload_model();
    REQUIRE(backend.is_available());

    const auto silent = backend.transcribe_audio(std::vector<float>(48000, 0.0f), "");
    CHECK(silent.text.empty());
    CHECK(silent.segments.empty());
    CHECK(fake->decodes().empty());

    const auto result = backend.transcribe_audio(std::vector<float>(61 * 16000, 0.1f), "");
    CHECK(result.text == "one one one");
    REQUIRE(result.segments.size() == 3);
    std::vector<double> starts;
    for (const auto& segment : result.segments) {
        starts.push_back(std::round(segment.start * 10) / 10);
    }
    CHECK(starts == std::vector<double>{0.0, 24.1, 48.2});
}

// from tests/test_local_asr.py::test_cancel_during_load_cannot_publish_stale_model
TEST_CASE("cancel during load cannot publish stale model", "[test_local_asr]") {
    auto fake = decoders(holding_loads());
    ParakeetBackend backend(parakeet_config("cpu"), fake->factory());
    auto loading = std::async(std::launch::async, [&] { backend.reload_model(); });
    REQUIRE(fake->wait_entered(2s));
    backend.cancel_transcription();
    fake->resume();
    loading.get();
    CHECK_FALSE(backend.is_available());
    CHECK_FALSE(backend.last_loaded_model());
    CHECK(fake->alive() == 0);
}

// from
// tests/test_local_transcription_contracts.py::test_lazy_failure_resets_busy_and_next_call_can_succeed
// The Whisper backend's lazy segments become Parakeet's windows: a cancel or a failure on the
// second window of three.
TEST_CASE("lazy failure resets busy and next call can succeed",
          "[test_local_transcription_contracts]") {
    const std::string failure = GENERATE("cancel", "decoder");
    auto fake = decoders();
    ParakeetBackend backend(parakeet_config(), fake->factory());
    backend.reload_model();
    std::vector<fv::speech::AudioClip> windows;
    for (int i = 0; i < 3; ++i) {
        windows.push_back(fv::speech::test::clip(std::vector<float>(16000, 0.2f), i * 1.0));
    }
    int calls = 0;
    FakeDecoders::Script watching;
    watching.on_transcribe = [&] {
        CHECK(backend.is_transcribing());
        if (++calls == 2) {
            if (failure == "cancel") {
                backend.cancel_transcription();
            } else {
                FakeDecoders::Script failing;
                failing.transcribe_error = "decoder failed";
                fake->set_script(failing);
            }
        }
    };
    fake->set_script(watching);
    CHECK_THROWS_WITH(backend.transcribe_windows(windows),
                      ContainsSubstring(failure == "cancel" ? "canceled" : "decoder failed"));
    CHECK_FALSE(backend.is_transcribing());

    if (failure == "cancel") {
        // A cancel closes the engine; the next job loads it again.
        CHECK_FALSE(backend.is_available());
        backend.reload_model();
    }
    fake->set_script(texts({"next"}));
    CHECK(backend.transcribe_windows(std::span(windows).first(1)) ==
          std::vector<std::string>{"next"});
}

// from tests/test_local_transcription_contracts.py::test_unavailable_model_is_not_transcribed
TEST_CASE("unavailable model is not transcribed", "[test_local_transcription_contracts]") {
    auto fake = decoders();
    ParakeetBackend backend(parakeet_config(), fake->factory());
    const std::vector<fv::speech::AudioClip> windows{
        fv::speech::test::clip(std::vector<float>(16000, 0.2f))};
    CHECK_THROWS_WITH(backend.transcribe_windows(windows), "Parakeet is not loaded");
    CHECK_FALSE(backend.is_transcribing());
    CHECK(fake->decodes().empty());
}

// from
// tests/test_local_asr_languages.py::test_requests_use_a_supported_language_even_before_controls_open
// The Parakeet row; the rows of Moonshine, Qwen, Nemotron and MLX are dropped engines.
TEST_CASE("requests use a supported language even before controls open",
          "[test_local_asr_languages]") {
    const auto [stored, expected] = GENERATE(table<std::string, std::string>({
        {"zh", "auto"},
        {"ru", "ru"},
        {"de", "auto"},
    }));
    auto config = parakeet_config();
    config.settings = std::make_shared<const Json>(Json{{"local_asr_language", stored}});
    ParakeetBackend backend(config, decoders()->factory());
    CHECK(backend.request_language() == expected);
}

TEST_CASE("a request without a language uses the engine's setting") {
    auto fake = decoders();
    auto config = parakeet_config();
    config.settings = std::make_shared<const Json>(Json{{"local_asr_language", "fr"}});
    ParakeetBackend backend(config, fake->factory());
    backend.reload_model();
    backend.transcribe_audio(std::vector<float>(1600, 0.2f), "");
    backend.transcribe_audio(std::vector<float>(1600, 0.2f), "auto");
    const auto decodes = fake->decodes();
    REQUIRE(decodes.size() == 2);
    CHECK(decodes[0].language == "fr");
    // "auto" goes to the decoder as it is; the recognizer sends no language code for it.
    CHECK(decodes[1].language == "auto");
    CHECK(backend.recognition_language("") == "fr");
    CHECK(backend.recognition_language("es") == "es");
    CHECK(backend.recognition_language("zh") == "auto");
    CHECK(backend.recognition_support() == "after");
}

TEST_CASE("a loaded engine reads as its label and device") {
    auto fake = decoders();
    ParakeetBackend backend(parakeet_config("cuda"), fake->factory());
    CHECK(backend.device_info() == "Parakeet is not loaded");
    const auto readiness = backend.load({});
    CHECK(readiness.state == ReadyState::Ready);
    CHECK(readiness.device_info == "Parakeet TDT 0.6B v3 | cuda");
    CHECK(readiness.message.empty());
    CHECK(backend.last_loaded_model() == "parakeet-v3");
    CHECK(fake->created_gpus() == std::vector<int>{0});
    CHECK(backend.name() == "Parakeet");
}

// CTest runs each test case in its own process, so this load is the process's first.
TEST_CASE("the first load names the runtime it loaded, once") {
    auto fake = decoders();
    fv::speech::test::LogCapture log;
    ParakeetBackend backend(parakeet_config("cpu"), fake->factory());
    backend.reload_model();
    backend.reload_model();
    const std::string line = "INFO transcriber.optional_backend: Loaded the NeMo-Speech.cpp CPU "
                             "runtime from runtime\n";
    const std::string text = log.text();
    const auto first = text.find(line);
    REQUIRE(first != std::string::npos);
    CHECK(text.find(line, first + 1) == std::string::npos);
}

TEST_CASE("a crashed engine's sentence names the log file") {
    CHECK(fv::speech::engine_stopped_message() ==
          "The speech engine stopped unexpectedly. Details are in fiorivoce.log.");
}

TEST_CASE("a missing model or runtime says what to download") {
    SECTION("model") {
        auto config = parakeet_config();
        config.missing_state = ReadyState::ModelMissing;
        ParakeetBackend backend(config, decoders()->factory());
        const auto readiness = backend.load({});
        CHECK(readiness.state == ReadyState::ModelMissing);
        CHECK(readiness.device_info == "Download Parakeet TDT 0.6B v3 in Downloads.");
        CHECK(backend.is_model_missing());
    }
    SECTION("runtime") {
        const std::string device = GENERATE("cuda", "cpu");
        auto config = parakeet_config(device);
        config.missing_state = ReadyState::RuntimeMissing;
        ParakeetBackend backend(config, decoders()->factory());
        const auto readiness = backend.load({});
        CHECK(readiness.state == ReadyState::RuntimeMissing);
        CHECK(readiness.device_info == (device == "cuda"
                                            ? "Install Parakeet's GPU runtime in Downloads."
                                            : "Install Parakeet's CPU runtime in Downloads."));
    }
    SECTION("the resolver's own sentence") {
        auto config = parakeet_config();
        config.missing_state = ReadyState::RuntimeMissing;
        config.missing_message = "Parakeet is not available on this computer. Too old.";
        ParakeetBackend backend(config, decoders()->factory());
        CHECK(backend.load({}).device_info == *config.missing_message);
        CHECK(backend.device_info() == *config.missing_message);
    }
}

TEST_CASE("an auto device falls back to the cpu when the gpu cannot load") {
    auto fake = decoders(gpu_failing("CUDA error: no kernel image is available."));
    ParakeetBackend backend(parakeet_config("cuda", "auto"), fake->factory());
    const auto readiness = backend.load({});
    CHECK(readiness.state == ReadyState::Ready);
    CHECK(readiness.device_info == "Parakeet TDT 0.6B v3 | cpu");
    CHECK(readiness.message ==
          "Parakeet is using the CPU. CUDA error: no kernel image is available.");
    CHECK(fake->created_gpus() == std::vector<int>{0, fv::kCpuDevice});
}

TEST_CASE("a chosen gpu that cannot load fails the load") {
    auto fake = decoders(gpu_failing("CUDA error: out of memory"));
    ParakeetBackend backend(parakeet_config("cuda", "cuda"), fake->factory());
    const auto readiness = backend.load({});
    CHECK(readiness.state == ReadyState::Failed);
    CHECK(readiness.message == "CUDA error: out of memory");
    CHECK(backend.device_info() == "CUDA error: out of memory");
    CHECK(fake->created_gpus() == std::vector<int>{0});
}

TEST_CASE("a model of another family is refused") {
    auto config = parakeet_config();
    config.backend = "api";
    ParakeetBackend backend(config, decoders()->factory());
    CHECK_THROWS_AS(backend.reload_model(), std::invalid_argument);
    CHECK_THROWS_WITH(backend.reload_model(), "Model does not belong to this backend");
}

TEST_CASE("a reload asked for after a cancel does nothing") {
    auto fake = decoders();
    ParakeetBackend backend(parakeet_config(), fake->factory());
    fv::CancelSource cancel;
    cancel.cancel();
    backend.reload_model(cancel.token());
    CHECK(fake->created_gpus().empty());
    CHECK(backend.generation() == 0);
}

TEST_CASE("every cleanup advances the generation") {
    auto fake = decoders();
    ParakeetBackend backend(parakeet_config(), fake->factory());
    backend.reload_model();
    const auto loaded = backend.generation();
    backend.cleanup();
    CHECK(backend.generation() == loaded + 1);
    CHECK_FALSE(backend.is_available());
    CHECK(fake->alive() == 0);
    backend.supersede();
    CHECK(backend.generation() == loaded + 2);
}

TEST_CASE("a decode finds the engine closed by a cancel on another thread") {
    auto fake = decoders(holding_decodes());
    ParakeetBackend backend(parakeet_config(), fake->factory());
    backend.reload_model();
    auto decoding = std::async(std::launch::async, [&] {
        return backend.transcribe_audio(std::vector<float>(16000, 0.2f), "en");
    });
    REQUIRE(fake->wait_entered(2s));
    backend.cancel_transcription();
    CHECK_FALSE(backend.is_available());
    fake->resume();
    CHECK_THROWS_WITH(decoding.get(), "Transcription canceled");
    // The decoder went with the operation that held it, on its thread.
    CHECK(fake->alive() == 0);
}

// transcribe_windows refuses a cancelled or unloaded engine before it looks at the audio, so
// silence cannot hide either (tests/test_qwen_silence.py's
// test_silence_cannot_hide_unloaded_or_canceled_backend, for Parakeet).
TEST_CASE("decode refuses an unloaded or cancelled engine even for a silent clip") {
    const auto silent = fv::speech::test::clip(std::vector<float>(16000, 0.0f));
    SECTION("never loaded") {
        auto fake = decoders();
        ParakeetBackend backend(parakeet_config(), fake->factory());
        CHECK_THROWS_WITH(backend.decode(fv::speech::test::job(silent)), "Parakeet is not loaded");
    }
    SECTION("model missing") {
        auto config = parakeet_config();
        config.missing_state = ReadyState::ModelMissing;
        ParakeetBackend backend(config, decoders()->factory());
        backend.load({});
        CHECK_THROWS_WITH(backend.decode(fv::speech::test::job(silent)),
                          "Download Parakeet TDT 0.6B v3 in Downloads.");
    }
    SECTION("cancelled") {
        auto fake = decoders();
        ParakeetBackend backend(parakeet_config(), fake->factory());
        backend.reload_model();
        auto cancelled = fv::speech::test::job(silent);
        fv::CancelSource cancel;
        cancel.cancel();
        cancelled.cancel = cancel.token();
        CHECK_THROWS_WITH(backend.decode(cancelled), "Transcription canceled");
        // Loaded and not cancelled, silence is simply empty.
        CHECK(backend.decode(fv::speech::test::job(silent)).transcript.text.empty());
        CHECK(fake->decodes().empty());
    }
}

// transcribe and decode_window send recognition_inputs' language; recognize() (the remote host)
// and SpeechDecoder (meetings) send the caller's.
TEST_CASE("a decode's language is chosen as the python path for its lane chooses it") {
    auto fake = decoders();
    auto config = parakeet_config();
    config.settings = std::make_shared<const Json>(Json{{"local_asr_language", "fr"}});
    ParakeetBackend backend(config, fake->factory());
    backend.reload_model();
    const auto audible = fv::speech::test::clip(std::vector<float>(1600, 0.2f));
    using fv::speech::Lane;
    const std::vector<std::tuple<Lane, std::string, std::string>> rows{
        {Lane::Final, "zh", "auto"},        {Lane::Final, "", "fr"},
        {Lane::Final, "es", "es"},          {Lane::Incremental, "de", "auto"},
        {Lane::Preview, "auto", "auto"},    {Lane::Meeting, "de", "de"},
        {Lane::Background, "auto", "auto"}, {Lane::RemoteClient, "zh", "zh"},
        {Lane::RemoteClient, "", "fr"},
    };
    for (const auto& [lane, language, sent] : rows) {
        backend.decode(fv::speech::test::job(audible, lane, language));
    }
    const auto decodes = fake->decodes();
    REQUIRE(decodes.size() == rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        INFO("row " << i);
        CHECK(decodes[i].language == std::get<2>(rows[i]));
    }
}

TEST_CASE("decode takes 16 kHz clips only") {
    auto fake = decoders();
    ParakeetBackend backend(parakeet_config(), fake->factory());
    backend.reload_model();
    auto clip = fv::speech::test::clip(std::vector<float>(100, 0.2f));
    clip.rate = 44100;
    CHECK_THROWS_AS(backend.decode(fv::speech::test::job(clip)), std::invalid_argument);
    fv::speech::DecodeJob file;
    file.input = std::filesystem::path("x.wav");
    CHECK_THROWS_AS(backend.decode(file), std::invalid_argument);
}
