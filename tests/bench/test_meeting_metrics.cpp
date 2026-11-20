// benchmarks/meeting_mode: the AMI reference parser and the scoring every benchmark reuses
// (tests/test_meeting_benchmark.py), and score_text's silence contract
// (tests/test_benchmark_contracts.py).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <utility>
#include <vector>

#include "bench/ami.h"
#include "bench/commands.h"
#include "bench/meeting_metrics.h"
#include "bench/meeting_run.h"
#include "bench_test_support.h"
#include "meeting_capture/spool.h"
#include "testkit/testkit.h"

using Catch::Approx;
using fv::Json;
using fv::bench::ReferenceWord;
using fv::bench::TimedText;

TEST_CASE("test_ami_reference_parser_merges_speakers_and_skips_events",
          "[test_meeting_benchmark]") {
    // from
    // tests/test_meeting_benchmark.py::test_ami_reference_parser_merges_speakers_and_skips_events
    fv::testkit::TempDataRoot root;
    fv::bench::test::write_file(root.path() / "words" / "TEST.A.words.xml", R"(<?xml version="1.0"?>
        <root xmlns:nite="http://nite.sourceforge.net/">
          <w starttime="2.0" endtime="2.2">Later</w>
          <vocalsound starttime="2.2" endtime="2.5" type="laugh" />
          <w starttime="2.5" endtime="2.5" punc="true">.</w>
        </root>)");
    fv::bench::test::write_file(root.path() / "words" / "TEST.B.words.xml", R"(<?xml version="1.0"?>
        <root xmlns:nite="http://nite.sourceforge.net/">
          <w starttime="1.0" endtime="1.2">First</w>
          <gap starttime="1.3" endtime="1.4" />
        </root>)");

    const auto parsed = fv::bench::parse_reference_words(root.path(), "TEST");

    std::vector<std::pair<std::string, std::string>> got;
    for (const ReferenceWord& word : parsed) {
        got.emplace_back(word.text, word.speaker);
    }
    CHECK(got == std::vector<std::pair<std::string, std::string>>{{"First", "B"}, {"Later", "A"}});
}

TEST_CASE("the AMI parser refuses a meeting without word files or usable words", "[ami]") {
    fv::testkit::TempDataRoot root;
    CHECK_THROWS_WITH(
        fv::bench::parse_reference_words(root.path(), "IN1001"),
        Catch::Matchers::ContainsSubstring("No AMI word annotations found for IN1001"));
    fv::bench::test::write_file(
        root.path() / "words" / "IN1001.A.words.xml",
        R"(<root><w starttime="x" endtime="1">bad</w><w>untimed</w></root>)");
    CHECK_THROWS_WITH(fv::bench::parse_reference_words(root.path(), "IN1001"),
                      "No usable AMI reference words found for IN1001");
    CHECK(fv::bench::select_meetings({" in1002 ", ""}).front().meeting_id == "IN1002");
    CHECK(fv::bench::select_meetings({}).size() == 10);
    CHECK_THROWS_WITH(fv::bench::select_meetings({"IN9999", "x"}),
                      "Unknown curated AMI meeting id(s): IN9999, X");
}

TEST_CASE("test_edit_counts_are_exact_and_directional", "[test_meeting_benchmark]") {
    // from tests/test_meeting_benchmark.py::test_edit_counts_are_exact_and_directional
    const std::vector<std::string> reference = {"the", "quick", "brown", "fox"};
    const std::vector<std::string> hypothesis = {"the", "slow", "fox", "today"};

    const auto counts = fv::bench::edit_counts(reference, hypothesis);

    CHECK(counts.substitutions == 1);
    CHECK(counts.deletions == 1);
    CHECK(counts.insertions == 1);
    CHECK(counts.errors() == 3);
}

TEST_CASE("test_timed_score_contains_alignment_drift_within_windows", "[test_meeting_benchmark]") {
    // from
    // tests/test_meeting_benchmark.py::test_timed_score_contains_alignment_drift_within_windows
    const std::vector<ReferenceWord> reference = {
        {"alpha", 1.0, 1.2, "A"},
        {"beta", 2.0, 2.2, "A"},
        {"gamma", 301.0, 301.2, "A"},
        {"delta", 302.0, 302.2, "A"},
    };
    const std::vector<TimedText> hypothesis = {
        {-5.0, -1.0, "outside before"},
        {1.0, 1.2, "alpha beta extra"},
        {301.0, 301.2, "gamma"},
        {400.0, 401.0, "outside after"},
    };

    const Json score = fv::bench::score_timed_transcript(reference, hypothesis, 300.0);

    CHECK(score["reference_words"] == 4);
    CHECK(score["hypothesis_words"] == 4);
    CHECK(score["deletions"] == 1);
    CHECK(score["insertions"] == 1);
    CHECK(score["substitutions"] == 0);
    CHECK(score["wer"].get<double>() == Approx(1.0 / 2));
    CHECK(score["windows"].size() == 2);
    CHECK(score["out_of_reference_hypothesis_words"] == 4);
    CHECK(score["metric"] == "tcWER-300s");
    CHECK_THROWS_AS(fv::bench::score_timed_transcript(reference, hypothesis, 0.0),
                    std::invalid_argument);
}

TEST_CASE("test_normalization_is_conservative", "[test_meeting_benchmark]") {
    // from tests/test_meeting_benchmark.py::test_normalization_is_conservative
    CHECK(fv::bench::normalize_tokens("It\u2019s GPU-based, A_S_R_ version 2.0!") ==
          std::vector<std::string>{"it's", "gpu", "based", "asr", "version", "2", "0"});
}

TEST_CASE("normalisation applies NFKC before lower-casing", "[meeting_metrics]") {
    // NFKC folds ligatures, full-width forms and superscripts, and composes a decomposed accent.
    CHECK(fv::bench::normalize_tokens("\uFB01ne \uFF27\uFF30\uFF35 x\u00B2 cafe\u0301 it`s") ==
          std::vector<std::string>{"fine", "gpu", "x2", "caf\u00E9", "it's"});
    CHECK(fv::bench::normalize_tokens("\uD55C\uAD6D\uC5B4 \u1112\u1161\u11AB") ==
          std::vector<std::string>{"\uD55C\uAD6D\uC5B4", "\uD55C"});
    CHECK(fv::bench::normalize_tokens("").empty());
}

TEST_CASE("test_reference_overlap_stats_counts_both_speakers", "[test_meeting_benchmark]") {
    // from tests/test_meeting_benchmark.py::test_reference_overlap_stats_counts_both_speakers
    const std::vector<ReferenceWord> reference = {
        {"one", 0.0, 1.0, "A"},
        {"two", 0.5, 1.5, "B"},
        {"three", 1.5, 2.0, "A"},
        {"four", 1.6, 1.8, "A"},
    };

    const Json stats = fv::bench::reference_overlap_stats(reference);

    CHECK(stats["reference_words"] == 4);
    CHECK(stats["overlapping_reference_words"] == 2);
    CHECK(stats["overlap_rate"].get<double>() == Approx(0.5));
}

TEST_CASE("test_benchmark_defaults_to_stable_production_profile", "[test_meeting_benchmark]") {
    // from tests/test_meeting_benchmark.py::test_benchmark_defaults_to_stable_production_profile
    // OpenWhisper pins its Whisper draft-prompt length; Parakeet takes no prompt, so
    // OpenWhisper_CPP's production profile is the live spool's chunking with the offline pass on.
    const fv::bench::MeetingArgs args = fv::bench::parse_meeting_args({});

    CHECK(args.target_sec == fv::meeting::DEFAULT_TARGET_SEC);
    CHECK(args.max_sec == fv::meeting::DEFAULT_MAX_SEC);
    CHECK(args.offline_pass);
    CHECK(args.language == "auto");
    CHECK_FALSE(fv::bench::parse_meeting_args({"--no-offline-pass"}).offline_pass);
    CHECK_THROWS_AS(fv::bench::parse_meeting_args({"--draft-prompt-words", "50"}),
                    fv::bench::UsageError);
}

TEST_CASE("test_silence_preserves_insertions_and_has_no_defined_wer",
          "[test_benchmark_contracts]") {
    // from
    // tests/test_benchmark_contracts.py::test_silence_preserves_insertions_and_has_no_defined_wer
    const auto score = fv::bench::score_text("", "invented words");

    CHECK(score.counts.insertions == 2);
    CHECK(score.errors() == 2);
    CHECK(score.words == 0);
    CHECK_FALSE(score.wer.has_value());
    CHECK(score.to_json()["wer"].is_null());
}

TEST_CASE("aggregate_scores sums counts and averages meeting WERs", "[meeting_metrics]") {
    const std::vector<Json> scores = {Json{{"reference_words", 10},
                                           {"hypothesis_words", 9},
                                           {"substitutions", 1},
                                           {"deletions", 1},
                                           {"insertions", 0},
                                           {"wer", 0.2}},
                                      Json{{"reference_words", 30},
                                           {"hypothesis_words", 30},
                                           {"substitutions", 0},
                                           {"deletions", 0},
                                           {"insertions", 2},
                                           {"wer", 2.0 / 30}}};
    const Json total = fv::bench::aggregate_scores(scores);
    CHECK(total["meetings"] == 2);
    CHECK(total["wer"].get<double>() == Approx(4.0 / 40));
    CHECK(total["macro_wer"].get<double>() == Approx((0.2 + 2.0 / 30) / 2));
    CHECK(fv::bench::aggregate_scores({})["wer"] == 0.0);
}
