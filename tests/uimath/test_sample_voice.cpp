// The golden vectors of ui_qt.widgets.recording_look_preview (the sample voice) and the port of
// the OpenWhisper test of it.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "overlay_test_support.h"
#include "uimath/sample_voice.h"

using fv::Json;

namespace {

constexpr char kModule[] = "ui_qt.widgets.recording_look_preview";

} // namespace

TEST_CASE("the sample words are the table random.Random(3) lays out", "[golden][sample_voice]") {
    fv::golden::for_each(kModule, "_sample_words", [](const fv::golden::Case& c) {
        Json words = Json::array();
        for (const fv::SampleWord& word : fv::sample_words()) {
            words.push_back(Json::array({word.start, word.length, word.peak}));
        }
        CHECK(fv::golden::matches(c.raw().at("result"), words, 0.0));
    });
}

TEST_CASE("sample_level replays the golden vectors", "[golden][sample_voice]") {
    int skipped = 0;
    fv::golden::for_each(kModule, "sample_level", [&](const fv::golden::Case& c) {
        if (c.raises()) {
            // Python's sin(...) ** 0.7 is a complex number when rounding leaves the sine a hair
            // below 0 at a word's last instant, and max() then raises TypeError. The port
            // clamps it to 0, so there is no value to compare.
            CHECK(c.raises_type() == "TypeError");
            ++skipped;
            return;
        }
        CHECK(fv::golden::near(c.result<double>(), fv::sample_level(c.arg<double>(0)), 1e-12));
    });
    CHECK(skipped <= 5);
}

TEST_CASE("the stages and the loop are Python's", "[golden][sample_voice]") {
    fv::golden::for_each(kModule, "tables", [](const fv::golden::Case& c) {
        const Json& result = c.raw().at("result");
        CHECK(result.at("loop_s").get<double>() == fv::kLoopS);
        CHECK(result.at("word_count").get<std::size_t>() == fv::sample_words().size());
        const Json& stages = result.at("stages");
        REQUIRE(stages.size() == fv::kStages.size());
        for (std::size_t i = 0; i < fv::kStages.size(); ++i) {
            CHECK(stages[i].at(0).get<std::string>() == fv::kStages[i].name);
            CHECK(stages[i].at(1).get<double>() == fv::kStages[i].start);
        }
    });
}

// from tests/test_recording_look.py::test_the_sample_voice_speaks_then_pauses
TEST_CASE("the sample voice speaks then pauses", "[test_recording_look][sample_voice]") {
    CHECK((fv::sample_level(1.0) > 0.02 || fv::sample_level(1.1) > 0.02));
    CHECK(fv::sample_level(5.5) < 0.01);
}

TEST_CASE("the sample voice loops and hums between words", "[sample_voice]") {
    // Two phrases (0.5-2.1 s and 2.9-4.6 s), then silence until the loop restarts.
    const auto& words = fv::sample_words();
    CHECK(words.front().start == 0.5);
    for (const fv::SampleWord& word : words) {
        CHECK(word.length >= 0.14);
        CHECK(word.length <= 0.32);
        CHECK(word.peak >= 0.04);
        CHECK(word.peak <= 0.16);
    }
    CHECK(fv::sample_level(2.5) < 0.0036); // the pause between phrases: the floor's hum
    CHECK(fv::sample_level(2.5) > 0.0014);
    CHECK(fv::golden::near(fv::sample_level(1.0 + fv::kLoopS), fv::sample_level(1.0), 1e-9));
    CHECK(fv::golden::near(fv::sample_level(1.0 - fv::kLoopS), fv::sample_level(1.0), 1e-9));
}
