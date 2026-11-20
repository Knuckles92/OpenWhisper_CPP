// benchmarks/live_preview.py over the production speech::WindowPreview on a scripted engine
// (tests/test_live_preview_benchmark.py), and on the real one when the decode variables are set.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "bench/commands.h"
#include "bench/live_preview.h"
#include "bench/provenance.h"
#include "bench_test_support.h"
#include "process/testing/map_env.h"
#include "testkit/testkit.h"

using Catch::Approx;
using fv::Json;
using fv::bench::test::ScriptedSpeech;
using fv::bench::test::tone;

namespace {

constexpr int kRate = 44100;

// time.perf_counter replaced by a script of readings, as the Python tests monkeypatch it.
std::function<double()> ticks(std::vector<double> values) {
    auto script = std::make_shared<std::vector<double>>(std::move(values));
    auto next = std::make_shared<std::size_t>(0);
    return [script, next] {
        if (*next >= script->size()) {
            throw std::logic_error("perf_counter read more often than scripted");
        }
        return (*script)[(*next)++];
    };
}

// int(frames * 16000 / 44100): the samples a window of `frames` reaches the engine with.
std::size_t resampled(std::int64_t frames) {
    return static_cast<std::size_t>(static_cast<double>(frames * 16000) / kRate);
}

} // namespace

TEST_CASE("test_callback_quantization_and_short_clip", "[test_live_preview_benchmark]") {
    // from tests/test_live_preview_benchmark.py::test_callback_quantization_and_short_clip
    CHECK(fv::bench::block_ends(7 * kRate, kRate, 3.0) ==
          std::vector<std::int64_t>{133120, 266240});
    CHECK(fv::bench::block_ends(kRate, kRate, 3.0).empty());
}

TEST_CASE("test_silence_counts_hallucinations_without_inventing_wer",
          "[test_live_preview_benchmark]") {
    // from
    // tests/test_live_preview_benchmark.py::test_silence_counts_hallucinations_without_inventing_wer
    const Json result = fv::bench::preview_score("", "Thank you");
    CHECK(result["insertions"] == 2);
    CHECK(result["wer"].is_null());
}

TEST_CASE("test_window_replay_includes_overlap_and_excludes_post_stop_text",
          "[test_live_preview_benchmark]") {
    // from
    // tests/test_live_preview_benchmark.py::test_window_replay_includes_overlap_and_excludes_post_stop_text
    // A tone, not zeros: FioriVoce's preview never sends digital silence to the engine.
    auto speech = std::make_shared<ScriptedSpeech>();
    speech->text = [](std::size_t call) {
        return std::vector<std::string>{"one", "two", "three"}.at(call - 1);
    };
    auto engine = fv::bench::test::scripted_engine(speech);
    fv::bench::ReplayOptions options;
    options.perf_counter = ticks({0.0, 0.1, 1.0, 1.2, 2.0, 2.3});

    const Json result =
        fv::bench::replay(engine->service(), tone(7 * kRate, kRate), "one two three", options);

    const auto seen = speech->call_lengths();
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == resampled(133120));
    CHECK(seen[1] == resampled(133120 + static_cast<std::int64_t>(0.75 * kRate)));
    // The tail after the last full window, with the overlap in front of it.
    CHECK(seen[2] == resampled(7 * kRate - 266240 + static_cast<std::int64_t>(0.75 * kRate)));
    CHECK(result["live_text"] == "one two");
    CHECK(result["drained_text"] == "one two three");
    CHECK(result["live"]["deletions"] == 1);
    CHECK(result["drained"]["errors"] == 0);
    CHECK(result["drain_after_stop_s"].get<double>() == Approx(0.3));
    CHECK(result["calls"].size() == 3);
    CHECK(result["calls"][2]["flush"].get<bool>());
    CHECK(result["first_text_before_stop_s"].get<double>() == Approx(133120.0 / kRate + 0.1));
}

TEST_CASE("test_slow_decode_models_backlog_and_visibility", "[test_live_preview_benchmark]") {
    // from tests/test_live_preview_benchmark.py::test_slow_decode_models_backlog_and_visibility
    auto speech = std::make_shared<ScriptedSpeech>();
    auto engine = fv::bench::test::scripted_engine(speech);
    fv::bench::ReplayOptions options;
    options.perf_counter = ticks({0.0, 4.0, 5.0, 9.0, 10.0, 14.0});

    const Json result =
        fv::bench::replay(engine->service(), tone(7 * kRate, kRate), "word word word", options);

    CHECK(result["live_text"] == ""); // The first completion is after the end of the audio.
    CHECK(result["calls"][1]["visible_s"].get<double>() == Approx(133120.0 / kRate + 8));
    CHECK(result["first_text_before_stop_s"].is_null());
    CHECK(fv::bench::aggregate({result}, 133120.0 / kRate)["update_deadline_misses"] == 2);
}

TEST_CASE("test_swallowed_production_exception_fails_benchmark", "[test_live_preview_benchmark]") {
    // from
    // tests/test_live_preview_benchmark.py::test_swallowed_production_exception_fails_benchmark
    auto speech = std::make_shared<ScriptedSpeech>();
    speech->text = [](std::size_t) -> std::string {
        throw std::runtime_error("inference failed");
    };
    auto engine = fv::bench::test::scripted_engine(speech);

    // A clip shorter than a window: the failure is in the finalized tail.
    CHECK_THROWS_WITH(fv::bench::replay(engine->service(), tone(4410, kRate), "hello"),
                      Catch::Matchers::ContainsSubstring("Preview decode failed"));
    // A live window fails.
    CHECK_THROWS_WITH(fv::bench::replay(engine->service(), tone(7 * kRate, kRate), "hello"),
                      Catch::Matchers::ContainsSubstring("Preview decode failed"));
}

TEST_CASE("a quiet window counts without reaching the engine", "[live_preview]") {
    auto speech = std::make_shared<ScriptedSpeech>();
    auto engine = fv::bench::test::scripted_engine(speech);
    const Json result =
        fv::bench::replay(engine->service(), std::vector<std::int16_t>(4 * kRate, 0), "", {});
    CHECK(speech->call_lengths().empty());
    CHECK(result["drained_text"] == "");
    CHECK(result["live"]["insertions"] == 0);
}

TEST_CASE("timed clips score prefix checkpoints every 3 s", "[live_preview]") {
    auto speech = std::make_shared<ScriptedSpeech>();
    speech->text = [](std::size_t call) {
        return call == 1 ? std::string("alpha") : "beta";
    };
    auto engine = fv::bench::test::scripted_engine(speech);
    fv::bench::ReplayOptions options;
    options.timed_words = Json::array(
        {Json{{"text", "alpha"}, {"end_s", 1.0}}, Json{{"text", "beta"}, {"end_s", 4.0}}});
    options.perf_counter = ticks({0.0, 0.1, 1.0, 1.1, 2.0, 2.1});
    const Json result =
        fv::bench::replay(engine->service(), tone(7 * kRate, kRate), "alpha beta", options);
    REQUIRE(result["checkpoints"].size() == 2);
    // At 3.02 s the reference holds "alpha" and the window decoded at 3.12 s is not visible yet.
    CHECK(result["checkpoints"][0]["words"] == 1);
    CHECK(result["checkpoints"][0]["deletions"] == 1);
    CHECK(result["checkpoints"][1]["words"] == 2);
    CHECK(result["checkpoints"][1]["errors"] == 1);
    CHECK(fv::bench::aggregate({result}, 3.0)["checkpoint_wer"].get<double>() == Approx(2.0 / 3));
}

TEST_CASE("test_aggregate_uses_word_weighted_errors", "[test_live_preview_benchmark]") {
    // from tests/test_live_preview_benchmark.py::test_aggregate_uses_word_weighted_errors
    std::vector<Json> clips;
    for (const auto& [ref, hyp] : std::vector<std::pair<std::string, std::string>>{
             {"one", ""}, {"one two three", "one two three"}}) {
        clips.push_back(Json{{"duration_s", 1.0},
                             {"calls", Json::array()},
                             {"first_text_before_stop_s", nullptr},
                             {"live_text", hyp},
                             {"live", fv::bench::preview_score(ref, hyp)},
                             {"drained", fv::bench::preview_score(ref, hyp)}});
    }
    const Json summary = fv::bench::aggregate(clips, 3.0);
    CHECK(summary["live"]["wer"] == 0.25);
    CHECK(summary["update_median_ms"].is_null());
    CHECK(summary["clips_without_live_text"] == 1);
}

TEST_CASE("percentile interpolates linearly as numpy does", "[live_preview]") {
    CHECK_FALSE(fv::bench::percentile({}, 50).has_value());
    CHECK(*fv::bench::percentile({3.0, 1.0, 2.0, 4.0}, 50) == Approx(2.5));
    CHECK(*fv::bench::percentile({1.0, 2.0, 3.0, 4.0, 5.0}, 95) == Approx(4.8));
}

TEST_CASE("test_rejects_invalid_benchmark_settings", "[test_live_preview_benchmark]") {
    // from tests/test_live_preview_benchmark.py::test_rejects_invalid_benchmark_settings
    const auto options = GENERATE(
        std::vector<std::string>{"--chunk", "0"}, std::vector<std::string>{"--chunk", "nan"},
        std::vector<std::string>{"--repeats", "0"}, std::vector<std::string>{"--overlap", "3"});
    std::vector<std::string> args{"manifest.json"};
    args.insert(args.end(), options.begin(), options.end());
    CHECK_THROWS_AS(fv::bench::parse_live_preview_args(args), fv::bench::UsageError);
    CHECK(fv::bench::parse_live_preview_args({"manifest.json"}).chunk == 3.0);
}

TEST_CASE("the live-preview command exits 2 naming a missing engine variable", "[live_preview]") {
    fv::MapEnvReader reader;
    fv::MapEnv env(false);
    std::ostringstream out;
    std::ostringstream err;
    fv::bench::CommandContext context{out, err, reader, env};
    CHECK(fv::bench::run_command({"live-preview", "manifest.json"}, context) == 2);
    CHECK_THAT(err.str(), Catch::Matchers::ContainsSubstring("Set FIORIVOCE_TEST_RUNTIME_DIR"));
    CHECK(fv::bench::run_command({"live-preview", "manifest.json", "--chunk", "0"}, context) == 2);
    CHECK_THAT(err.str(), Catch::Matchers::ContainsSubstring(
                              "error: chunk must be positive and overlap must be in [0, chunk)"));
}

TEST_CASE("the live-preview command measures the real engine on the test recording", "[decode]") {
    const auto runtime = fv::env_utf8("FIORIVOCE_TEST_RUNTIME_DIR");
    const auto model = fv::env_utf8("FIORIVOCE_TEST_MODEL");
    const auto wav = fv::env_utf8("FIORIVOCE_TEST_WAV");
    if (!runtime || !model || !wav) {
        SKIP("Set FIORIVOCE_TEST_RUNTIME_DIR, FIORIVOCE_TEST_MODEL and FIORIVOCE_TEST_WAV.");
    }
    fv::testkit::TempDataRoot root;
    const auto manifest = root.path() / "manifest.json";
    // jfk.wav, the decode tests' recording; another recording scores worse but still runs.
    fv::bench::write_json(
        manifest,
        Json{{"source", "test"},
             {"clips", Json::array({Json{{"audio_path", *wav},
                                         {"id", "test"},
                                         {"reference", "And so my fellow Americans, ask not what "
                                                       "your country can do for you, ask what you "
                                                       "can do for your country."}}})}});
    const auto output = root.path() / "bench-live-preview.json";
    fv::MapEnv env(false);
    std::ostringstream out;
    std::ostringstream err;
    fv::bench::CommandContext context{out, err, fv::system_env(), env};
    const int code =
        fv::bench::run_command({"live-preview", fv::path_to_utf8(manifest), "--repeats", "1",
                                "--output", fv::path_to_utf8(output)},
                               context);
    INFO(out.str() << err.str());
    REQUIRE(code == 0);
    const Json report = fv::bench::read_json(output);
    CHECK(report["schema"] == 1);
    CHECK(report["kind"] == "live-preview");
    const Json& summary = report["metrics"]["summary"];
    CHECK(summary["clips"] == 1);
    CHECK(summary["calls"].get<int>() >= 1);
    CHECK_FALSE(summary["drained"]["wer"].is_null());
    CHECK(summary["drained"]["wer"].get<double>() < 0.5);
    CHECK_FALSE(report["provenance"]["model"].is_null());
}
