// scripts/benchmark_local_asr.py and benchmark_local_asr_corpus.py: the corpus contracts of
// tests/test_benchmark_contracts.py on a scripted engine, and the asr command on the real one.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "bench/commands.h"
#include "bench/local_asr.h"
#include "bench/provenance.h"
#include "bench_test_support.h"
#include "process/testing/map_env.h"
#include "testkit/testkit.h"

using fv::Json;

namespace {

struct FakeAsr {
    bool fail = false;
    int cleanups = 0;
    std::vector<std::optional<std::string>> offline_seen;
    fv::MutableEnv* env = nullptr;

    fv::bench::AsrBackendLoader loader() {
        return [this] {
            fv::bench::AsrBackend backend;
            backend.device = "cpu";
            backend.device_info = "fake";
            backend.identity = Json{{"fake", true}};
            backend.transcribe = [this](const std::filesystem::path&) -> std::string {
                offline_seen.push_back(env->get("HF_HUB_OFFLINE"));
                if (fail) {
                    throw std::runtime_error("decode failed");
                }
                return "invented words";
            };
            backend.cleanup = [this] {
                ++cleanups;
            };
            return backend;
        };
    }
};

} // namespace

TEST_CASE("test_corpus_counts_silence_and_propagates_failures", "[test_benchmark_contracts]") {
    // from tests/test_benchmark_contracts.py::test_corpus_counts_silence_and_propagates_failures
    const bool failure = GENERATE(false, true);
    const auto previous = GENERATE(std::optional<std::string>(), std::optional<std::string>("0"));
    CAPTURE(failure, previous.value_or("<unset>"));
    fv::testkit::TempDataRoot root;
    fv::MapEnv env(false);
    if (previous) {
        env.set("HF_HUB_OFFLINE", *previous);
    }
    fv::bench::test::write_wav(root.path() / "silence.wav", std::vector<std::int16_t>(16000, 0),
                               16000);
    const auto manifest = root.path() / "manifest.json";
    fv::bench::write_json(manifest, Json{{"clips", Json::array({Json{{"audio_path", "silence.wav"},
                                                                     {"reference", ""},
                                                                     {"group", "silence"}}})}});
    const auto output = root.path() / "result.json";
    FakeAsr asr;
    asr.fail = failure;
    asr.env = &env;

    const int code =
        fv::bench::run_asr_corpus({manifest, output, "cpu", std::nullopt}, asr.loader(), env);

    CHECK(env.get("HF_HUB_OFFLINE") == previous);
    REQUIRE_FALSE(asr.offline_seen.empty());
    for (const auto& seen : asr.offline_seen) {
        CHECK(seen == "1");
    }
    const Json row = fv::bench::read_json(output)["results"][0];
    CHECK(code == static_cast<int>(failure));
    CHECK(asr.cleanups == 1);
    if (failure) {
        CHECK_THAT(row["error"].get<std::string>(),
                   Catch::Matchers::ContainsSubstring("decode failed"));
    } else {
        CHECK(row["groups"]["silence"]["insertions"] == 2);
        CHECK(row["groups"]["silence"]["errors"] == 2);
        CHECK(row["groups"]["silence"]["normalized_wer"].is_null());
        CHECK(row["aggregate"]["audio_s"] == 1.0);
    }
}

TEST_CASE("test_corpus_restores_offline_environment_after_invalid_input",
          "[test_benchmark_contracts]") {
    // from
    // tests/test_benchmark_contracts.py::test_corpus_restores_offline_environment_after_invalid_input
    fv::testkit::TempDataRoot root;
    fv::MapEnv env(false);
    env.set("HF_HUB_OFFLINE", "0");
    const auto manifest = root.path() / "invalid.json";
    fv::bench::test::write_file(manifest, "{");
    FakeAsr asr;
    asr.env = &env;

    CHECK_THROWS_AS(
        fv::bench::run_asr_corpus({manifest, root.path() / "out.json", "cpu", std::nullopt},
                                  asr.loader(), env),
        Json::parse_error);
    CHECK(env.get("HF_HUB_OFFLINE") == "0");
}

TEST_CASE("run_asr times a cold and warm decodes per file and scores a reference", "[local_asr]") {
    fv::testkit::TempDataRoot root;
    const auto wav = root.path() / "clip.wav";
    fv::bench::test::write_wav(wav, fv::bench::test::tone(32000, 16000), 16000);
    const auto reference = root.path() / "clip.txt";
    fv::bench::test::write_file(reference, "\xEF\xBB\xBFinvented words here");
    fv::MapEnv env(false);
    FakeAsr asr;
    asr.env = &env;
    fv::bench::AsrArgs args;
    args.audio = {wav};
    args.references = {reference};
    args.runs = 2;

    const auto run = fv::bench::run_asr(args, asr.loader());

    CHECK_FALSE(run.failed);
    CHECK(asr.offline_seen.size() == 3);
    CHECK(asr.cleanups == 1);
    const Json& file = run.metrics["files"][0];
    CHECK(file["duration_s"] == 2.0);
    CHECK(file["warm_s"].size() == 2);
    CHECK(file["transcript"] == "invented words");
    CHECK(file["score"]["deletions"] == 1);
    CHECK(file["normalized_wer"].get<double>() == 1.0 / 3);
    CHECK(run.metrics.contains("peak_process_rss_mb"));

    asr.fail = true;
    const auto failed = fv::bench::run_asr(args, asr.loader());
    CHECK(failed.failed);
    CHECK(failed.metrics["error"] == "decode failed");
    CHECK(asr.cleanups == 2);
    args.runs = 0;
    CHECK_THROWS_AS(fv::bench::run_asr(args, asr.loader()), std::invalid_argument);
}

TEST_CASE("the asr command exits 2 naming the variable that is missing", "[local_asr]") {
    fv::MapEnv env(false);
    std::ostringstream out;
    std::ostringstream err;
    fv::MapEnvReader runtime_only{{"OPENWHISPER_CPP_TEST_RUNTIME_DIR", "runtime"}};
    fv::bench::CommandContext context{out, err, runtime_only, env};
    CHECK(fv::bench::run_command({"asr", "clip.wav"}, context) == 2);
    CHECK(err.str() ==
          "Set OPENWHISPER_CPP_TEST_MODEL (or pass --model) to a Parakeet .gguf model.\n");
    fv::MapEnvReader none;
    fv::bench::CommandContext bare{out, err, none, env};
    CHECK(fv::bench::run_command({"asr-corpus", "manifest.json"}, bare) == 2);
    CHECK_THAT(err.str(), Catch::Matchers::ContainsSubstring(
                              "Set OPENWHISPER_CPP_TEST_RUNTIME_DIR (or pass --runtime)"));
    CHECK(fv::bench::run_command({"asr"}, bare) == 2);
    CHECK_THAT(err.str(), Catch::Matchers::ContainsSubstring("required: audio"));
    CHECK(fv::bench::run_command({"nonsense"}, bare) == 2);
}

TEST_CASE("the asr command reports first load, p95 and real-time factor on the real engine",
          "[decode]") {
    const auto runtime = fv::env_utf8("OPENWHISPER_CPP_TEST_RUNTIME_DIR");
    const auto model = fv::env_utf8("OPENWHISPER_CPP_TEST_MODEL");
    const auto wav = fv::env_utf8("OPENWHISPER_CPP_TEST_WAV");
    if (!runtime || !model || !wav) {
        SKIP("Set OPENWHISPER_CPP_TEST_RUNTIME_DIR, OPENWHISPER_CPP_TEST_MODEL and "
             "OPENWHISPER_CPP_TEST_WAV.");
    }
    fv::testkit::TempDataRoot root;
    const auto output = root.path() / "bench-asr.json";
    fv::MapEnv env(false);
    std::ostringstream out;
    std::ostringstream err;
    fv::bench::CommandContext context{out, err, fv::system_env(), env};

    const int code = fv::bench::run_command(
        {"asr", *wav, "--runs", "2", "--output", fv::path_to_utf8(output)}, context);

    INFO(out.str() << err.str());
    REQUIRE(code == 0);
    const Json report = fv::bench::read_json(output);
    CHECK(report["kind"] == "asr");
    const Json& metrics = report["metrics"];
    CHECK(metrics["load_s"].get<double>() > 0.0);
    CHECK(metrics["actual_device"] == "cpu");
    const Json& file = metrics["files"][0];
    CHECK(file["first_decode_s"].get<double>() > 0.0);
    CHECK(file["warm_p95_s"].get<double>() > 0.0);
    CHECK(file["real_time_factor"].get<double>() > 0.0);
    CHECK_FALSE(file["transcript"].get<std::string>().empty());
}
