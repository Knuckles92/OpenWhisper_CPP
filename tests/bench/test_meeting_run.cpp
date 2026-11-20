// benchmarks/meeting_mode/run.py and benchmarks/provenance.py: what a meeting benchmark result
// must hold to count (tests/test_benchmark_contracts.py), and the replay itself on a scripted
// engine.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

#include "base/hash.h"
#include "bench/live_preview.h"
#include "bench/meeting_run.h"
#include "bench/provenance.h"
#include "bench_test_support.h"
#include "testkit/testkit.h"

using fv::Json;
using fv::bench::test::write_file;

namespace {

// The scored_meeting fixture: "hello world" against the reference hello (0-1 s), world (1-2 s).
struct ScoredMeeting {
    ScoredMeeting() {
        write_file(root.path() / "words" / "synthetic.A.words.xml",
                   R"(<root><w starttime="0" endtime="1">hello</w>)"
                   R"(<w starttime="1" endtime="2">world</w></root>)");
    }

    Json make(Json offline, bool enabled = true) const {
        const Json segment{{"start_s", 0}, {"end_s", 2}, {"text", "hello world"}};
        return fv::bench::score_result(Json{{"meeting_id", "synthetic"},
                                            {"duration_s", 3600},
                                            {"elapsed_s", 1},
                                            {"chunks", 1},
                                            {"rtf", 1.0 / 3600},
                                            {"draft_segments", Json::array({segment})},
                                            {"final_segments", Json::array({segment})},
                                            {"offline_segments", std::move(offline)},
                                            {"run_offline", enabled}},
                                       root.path());
    }

    fv::testkit::TempDataRoot root;
};

std::vector<Json> copies(const Json& result, std::size_t count) {
    return std::vector<Json>(count, result);
}

} // namespace

TEST_CASE("test_empty_offline_is_scored_as_deletions_and_fails_gate",
          "[test_benchmark_contracts]") {
    // from
    // tests/test_benchmark_contracts.py::test_empty_offline_is_scored_as_deletions_and_fails_gate
    ScoredMeeting meeting;
    const Json result = meeting.make(Json::array());
    CHECK(result["offline_score"]["deletions"] == 2);

    const Json summary = fv::bench::meeting_summary(copies(result, 10), "fake", "en", 5, 20, true);

    CHECK(summary["quality_gate"]["product"] == "offline");
    CHECK_FALSE(summary["quality_gate"]["passed"].get<bool>());
    CHECK(summary["offline"]["wer"] == 1.0);
    // Ten hours of meetings and a fast run: only the offline transcript fails the gate.
    CHECK(summary["quality_gate"]["checks"]["meeting_count"].get<bool>());
    CHECK(summary["quality_gate"]["checks"]["audio_hours"].get<bool>());
    CHECK_FALSE(summary["quality_gate"]["checks"]["micro_tcwer"].get<bool>());
}

TEST_CASE("test_disabled_offline_does_not_use_cached_offline_score", "[test_benchmark_contracts]") {
    // from
    // tests/test_benchmark_contracts.py::test_disabled_offline_does_not_use_cached_offline_score
    ScoredMeeting meeting;
    const Json result = meeting.make(Json::array(), /*enabled=*/false);
    CHECK(result["offline_score"].is_null());

    const Json summary = fv::bench::meeting_summary(copies(result, 10), "fake", "en", 5, 20, false);

    CHECK(summary["quality_gate"]["passed"].get<bool>());
    CHECK(summary["quality_gate"]["product"] == "final");
    CHECK(summary["offline"].is_null());
}

TEST_CASE("test_missing_required_offline_score_cannot_substitute_draft",
          "[test_benchmark_contracts]") {
    // from
    // tests/test_benchmark_contracts.py::test_missing_required_offline_score_cannot_substitute_draft
    ScoredMeeting meeting;
    Json result = meeting.make(Json::array());
    result["offline_score"] = nullptr;

    CHECK_THROWS_WITH(fv::bench::meeting_summary(copies(result, 1), "fake", "en", 5, 20, true),
                      Catch::Matchers::ContainsSubstring("offline"));
}

TEST_CASE("the meeting summary writes its JSON and Markdown", "[meeting_run]") {
    ScoredMeeting meeting;
    const Json result =
        meeting.make(Json::array({Json{{"start_s", 0}, {"end_s", 2}, {"text", "hello world"}}}));
    const Json summary =
        fv::bench::meeting_summary(copies(result, 2), "parakeet-v3", "auto", 5, 20, true);
    fv::bench::write_meeting_summary(meeting.root.path() / "out", summary);

    CHECK(fv::bench::read_json(meeting.root.path() / "out" / "summary.json") == summary);
    const std::string md = fv::bench::read_text_file(meeting.root.path() / "out" / "summary.md");
    CHECK_THAT(md, Catch::Matchers::ContainsSubstring("- Offline tcWER: 0.00%"));
    CHECK_THAT(md, Catch::Matchers::ContainsSubstring("| synthetic | 60.0 | 1 | 0.00% | 0.00% | "
                                                      "0.00% | 0.000 | 0.000 |"));
    CHECK_THAT(md,
               Catch::Matchers::ContainsSubstring("Exceptional-quality gate (offline): `FAIL`"));
}

TEST_CASE("decode_meeting replays the spool's chunks, skips digital silence and runs the "
          "offline pass",
          "[meeting_run]") {
    fv::testkit::TempDataRoot root;
    // 7 s of tone, 1 s of digital silence, 7 s of tone at 16 kHz: the cut lands in the quiet.
    std::vector<std::int16_t> audio = fv::bench::test::tone(7 * 16000, 16000);
    audio.resize(audio.size() + 16000, 0);
    const auto tail = fv::bench::test::tone(7 * 16000, 16000);
    audio.insert(audio.end(), tail.begin(), tail.end());
    const auto wav = root.path() / "IN1001.Mix-Headset.wav";
    fv::bench::test::write_wav(wav, audio, 16000);

    std::vector<std::size_t> draft_lengths;
    std::vector<std::size_t> offline_lengths;
    fv::bench::MeetingDecoders decoders;
    decoders.draft = [&](std::vector<float> samples) {
        draft_lengths.push_back(samples.size());
        return std::vector<fv::Segment>{{" live words ", 0.0, 1.0}};
    };
    decoders.offline = [&](std::vector<float> samples) {
        offline_lengths.push_back(samples.size());
        return std::vector<fv::Segment>{{"clean words", 0.0, 1.0}};
    };
    const fv::bench::MeetingSpec spec{"IN1001", "test"};

    const Json result = fv::bench::decode_meeting(decoders, spec, wav, 5.0, 20.0, true);

    const auto ranges = fv::bench::chunk_ranges(audio, 16000, 5.0, 20.0);
    REQUIRE(ranges.size() >= 2);
    CHECK(ranges.front().first == 0);
    CHECK(ranges.back().second == static_cast<std::int64_t>(audio.size()));
    CHECK(result["chunks"] == ranges.size());
    CHECK(draft_lengths.size() <= ranges.size());
    CHECK_FALSE(draft_lengths.empty());
    CHECK(result["draft_segments"].size() == draft_lengths.size());
    CHECK(result["draft_segments"][0]["text"] == "live words");
    CHECK(result["draft_segments"][0]["channel"] == "loopback");
    CHECK(result["final_segments"] == result["draft_segments"]);
    CHECK_FALSE(offline_lengths.empty());
    CHECK(result["offline_segments"][0]["text"] == "clean words");
    CHECK(result["duration_s"] == 15.0);
    CHECK(result["run_offline"].get<bool>());

    decoders.offline = nullptr;
    CHECK_THROWS_WITH(fv::bench::decode_meeting(decoders, spec, wav, 5.0, 20.0, true),
                      "Required offline ASR model is unavailable");
    CHECK(fv::bench::decode_meeting(decoders, spec, wav, 5.0, 20.0, false)["offline_segments"]
              .empty());
}

TEST_CASE("test_provenance_rejects_legacy_changed_audio_settings_and_dirty_source",
          "[test_benchmark_contracts]") {
    // from
    // tests/test_benchmark_contracts.py::test_provenance_rejects_legacy_changed_audio_settings_and_dirty_source
    // OpenWhisper's config.py is OpenWhisper_CPP's settings key table, one of the hashed sources.
    fv::testkit::TempDataRoot root;
    const auto config = root.path() / "src" / "settings" / "keys.inc";
    write_file(config, "one");
    const auto audio = root.path() / "audio.wav";
    write_file(audio, "one");
    const auto stamp = [&](const std::string& model) {
        fv::bench::IdentityRequest request;
        request.source_root = root.path();
        request.inputs = {audio};
        request.settings = Json{{"model", model}};
        return fv::bench::identity(request);
    };
    const Json first = stamp("base");
    CHECK_FALSE(fv::bench::reusable(Json::object(), first));
    CHECK(fv::bench::reusable(Json{{"provenance", first}}, first));
    CHECK_FALSE(fv::bench::reusable(Json{{"provenance", first}, {"error", "failed"}}, first));

    const std::string change = GENERATE("audio", "settings", "source");
    CAPTURE(change);
    write_file(audio, change == "audio" ? "two" : "one");
    write_file(config, change == "source" ? "two" : "one");
    const Json stamped = stamp(change == "settings" ? "tiny" : "base");
    CHECK_FALSE(fv::bench::reusable(Json{{"provenance", first}}, stamped));
}

TEST_CASE("provenance hashes the benchmark's source tree and names the checkout's HEAD",
          "[provenance]") {
    fv::testkit::TempDataRoot root;
    write_file(root.path() / "src" / "settings" / "keys.inc", "keys");
    write_file(root.path() / "src" / "bench" / "a.cpp", "a");
    write_file(root.path() / "src" / "bench" / "notes.txt", "ignored");
    write_file(root.path() / ".git" / "HEAD", "ref: refs/heads/main\n");
    write_file(root.path() / ".git" / "refs" / "heads" / "main", "0123abcd\n");
    fv::bench::IdentityRequest request;
    request.source_root = root.path();
    const Json stamp = fv::bench::identity(request);
    CHECK(stamp["schema"] == 1);
    CHECK(stamp["source"].contains("src/bench/a.cpp"));
    CHECK_FALSE(stamp["source"].contains("src/bench/notes.txt"));
    CHECK(stamp["source"]["src/settings/keys.inc"] == fv::sha256_hex("keys"));

    const Json block = fv::bench::report_provenance(root.path(), std::nullopt, std::nullopt);
    CHECK(block["commit"] == "0123abcd");
    CHECK(block["runtime"].is_null());
    CHECK(block["model"].is_null());
    CHECK(block["machine"].contains("cpu_count"));
    const Json report = fv::bench::make_report("asr", Json{{"x", 1}}, block);
    CHECK(report["schema"] == 1);
    CHECK(report["kind"] == "asr");
}

TEST_CASE("test_preview_pool_rejects_different_installed_identity", "[test_benchmark_contracts]") {
    // from
    // tests/test_benchmark_contracts.py::test_preview_pool_rejects_different_installed_identity
    // OpenWhisper's installed identity (artifacts, model snapshot, runtime manifest, device
    // info) is OpenWhisper_CPP's runtime folder, model file and device info.
    const Json pass{{"summary", Json::object()}, {"clips", Json::array()}};
    const Json report{
        {"schema", 1},
        {"kind", "live-preview"},
        {"metrics", Json{{"settings", Json{{"chunk_s", 3.0}, {"effective_cadence_s", 1.0}}},
                         {"corpus", "same"},
                         {"source", "same"},
                         {"passes", Json::array({pass})},
                         {"load_s", 1.0},
                         {"device_info", "cpu"}}},
        {"provenance",
         Json{{"runtime", Json{{"path", "a"}}},
              {"model", Json{{"path", "a"}, {"bytes", 1}}},
              {"machine", Json{{"platform", "win32"}, {"machine", "AMD64"}, {"cpu_count", 8}}}}}};
    CHECK(fv::bench::pool_live_preview_reports({report, report})["passes"] == 2);

    const auto [field, path] = GENERATE(table<std::string, std::vector<std::string>>(
        {{"runtime", {"provenance", "runtime"}},
         {"model", {"provenance", "model"}},
         {"device_info", {"metrics", "device_info"}}}));
    CAPTURE(field);
    Json other = report;
    Json* node = &other;
    for (const std::string& key : path) {
        node = &(*node)[key];
    }
    *node = "different";
    CHECK_THROWS_WITH(fv::bench::pool_live_preview_reports({report, other}),
                      Catch::Matchers::ContainsSubstring(field));
}

TEST_CASE("pooling refuses different settings, corpus or hardware", "[live_preview]") {
    const Json base{
        {"metrics", Json{{"settings", Json{{"chunk_s", 3.0}}}, {"corpus", "a"}, {"source", "s"}}},
        {"provenance", Json{{"machine", Json{{"cpu_count", 8}}}}}};
    CHECK_THROWS_WITH(fv::bench::pool_live_preview_reports({}), "At least one report is required");
    Json other = base;
    other["metrics"]["settings"]["chunk_s"] = 2.0;
    CHECK_THROWS_WITH(fv::bench::pool_live_preview_reports({base, other}),
                      "Cannot pool runs with different chunk_s");
    other = base;
    other["metrics"]["corpus"] = "b";
    CHECK_THROWS_WITH(fv::bench::pool_live_preview_reports({base, other}),
                      "Cannot pool runs with different corpus");
    other = base;
    other["provenance"]["machine"]["cpu_count"] = 4;
    CHECK_THROWS_WITH(fv::bench::pool_live_preview_reports({base, other}),
                      "Cannot pool runs with different hardware cpu_count");
}
