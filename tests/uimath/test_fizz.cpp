// The golden vectors of ui_qt.overlays.ocean_looks.Fizz and the ports of the OpenWhisper tests
// that run on it. Python's random.Random(11) stream is replayed, so a run is a vector.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <numeric>
#include <string>

#include "overlay_test_support.h"
#include "uimath/fizz.h"
#include "uimath/voice_ribbon.h"

using fv::FizzSim;
using fv::Json;
using fv::overlay_test::matches_result;

namespace {

constexpr char kModule[] = "ui_qt.overlays.ocean_looks";
constexpr double kSpeech = 0.06;
constexpr double kSilence = 0.002;

double sum_of(const FizzSim::Levels& levels) {
    return std::accumulate(levels.begin(), levels.end(), 0.0);
}

Json levels_json(const FizzSim::Levels& levels) {
    return Json(std::vector<double>(levels.begin(), levels.end()));
}

Json state_json(const FizzSim& fizz) {
    Json bubbles = Json::array();
    for (const FizzSim::Bubble& b : fizz.bubbles()) {
        bubbles.push_back(Json::array({b.lane, b.x, b.y, b.vy, b.r}));
    }
    Json splashes = Json::array();
    for (const FizzSim::Splash& s : fizz.splashes()) {
        splashes.push_back(Json::array({s.start, s.x, s.y}));
    }
    return Json{{"h", levels_json(fizz.levels())},
                {"v", levels_json(fizz.velocity())},
                {"bubbles", bubbles},
                {"splashes", splashes},
                {"t", fizz.time()},
                {"carry", fizz.carry()}};
}

// The overlay's frame loop on a Fizz: the ribbon hears the level, then both advance a frame.
struct Rig {
    void run(double seconds, double level) {
        const double end = time + seconds;
        while (time < end) {
            ribbon.hear(level);
            ribbon.advance(kFrame);
            fizz.advance(kFrame, ribbon.shown());
            time += kFrame;
        }
    }

    static constexpr double kFrame = 1.0 / 30.0;
    fv::VoiceRibbon ribbon;
    FizzSim fizz;
    double time = 0.0;
};

} // namespace

TEST_CASE("a scripted run of Fizz replays the golden frames", "[golden][fizz]") {
    fv::golden::for_each(kModule, "Fizz.run", [](const fv::golden::Case& c) {
        FizzSim fizz(c.arg<double>(0));
        Json samples = Json::array();
        for (const Json& frame : c.args().at(1)) {
            fizz.advance(fv::golden::decode<double>(frame.at(1)),
                         fv::golden::decode<double>(frame.at(0)));
            const auto& h = fizz.levels();
            samples.push_back(Json::array({sum_of(h) / 9.0, *std::max_element(h.begin(), h.end()),
                                           fizz.bubbles().size(), fizz.splashes().size()}));
        }
        Json work_samples = Json::array();
        const Json& work = c.args().at(2);
        if (!work.empty()) {
            fizz.begin_work();
            const Json& draws = c.args().at(3);
            for (std::size_t i = 0; i < work.size(); ++i) {
                fizz.advance_work(fv::golden::decode<double>(work[i].at(0)),
                                  fv::golden::decode<double>(work[i].at(1)));
                const auto& h = fizz.work_levels(fv::golden::decode<double>(draws[i].at(0)),
                                                 fv::golden::decode<double>(draws[i].at(1)));
                Json row = levels_json(h);
                row.push_back(fizz.bubbles().size());
                row.push_back(fizz.splashes().size());
                work_samples.push_back(row);
            }
        }
        const Json actual{
            {"samples", samples}, {"work_samples", work_samples}, {"state", state_json(fizz)}};
        CHECK(matches_result(c, actual));
    });
}

TEST_CASE("Fizz's constants are Python's", "[golden][fizz]") {
    fv::golden::for_each(kModule, "tables", [](const fv::golden::Case& c) {
        const Json& fizz = c.raw().at("result").at("fizz");
        CHECK(fizz.at("LANES").get<int>() == FizzSim::kLanes);
        CHECK(fizz.at("BASE").get<double>() == FizzSim::kBase);
        CHECK(fizz.at("TOP").get<double>() == FizzSim::kTop);
        CHECK(fizz.at("KICK").get<double>() == FizzSim::kKick);
        CHECK(fizz.at("GRAVITY").get<double>() == FizzSim::kGravity);
        CHECK(fizz.at("LEAK").get<double>() == FizzSim::kLeak);
        CHECK(fizz.at("SLOSH_S").get<double>() == FizzSim::kSloshS);
        CHECK(fizz.at("MAX_H").get<double>() == FizzSim::kMaxHeight);
    });
}

// from tests/test_recording_look.py::test_fizz_fills_further_for_a_louder_voice
TEST_CASE("Fizz fills further for a louder voice", "[test_recording_look][fizz]") {
    double fills[2];
    int i = 0;
    for (const double level : {0.015, 0.06}) { // quiet speech, then ordinary speech
        Rig rig;
        rig.run(1.5, level);
        fills[i++] = sum_of(rig.fizz.levels()) / 9.0 / FizzSim::kMaxHeight;
    }
    INFO("the level shows how loud, not just that someone spoke");
    CHECK(fills[0] < 0.45);
    CHECK(fills[1] > 0.6);
}

// from tests/test_recording_look.py::test_fizz_fills_with_the_voice_and_drains_in_silence
TEST_CASE("Fizz fills with the voice and drains in silence", "[test_recording_look][fizz]") {
    Rig rig;
    rig.run(1.5, kSpeech);
    const double full = sum_of(rig.fizz.levels());
    rig.run(1.5, kSilence);
    CHECK(sum_of(rig.fizz.levels()) < full / 2);
}

TEST_CASE("a new recording empties the glasses and rewinds the bubbles", "[fizz]") {
    Rig rig;
    rig.run(0.5, 0.2);
    const Json first_run = state_json(rig.fizz);
    rig.fizz.reset();
    CHECK(sum_of(rig.fizz.levels()) == 0.0);
    CHECK(rig.fizz.bubbles().empty());
    CHECK(rig.fizz.splashes().empty());
    CHECK(rig.fizz.time() == 0.0);
    // The stream restarts: the same input gives the same bubbles.
    Rig again;
    again.run(0.5, 0.2);
    CHECK(state_json(again.fizz) == first_run);
}

TEST_CASE("the glasses never fill past the brim and the bubbles never rise off the top", "[fizz]") {
    Rig rig;
    for (int burst = 0; burst < 4; ++burst) {
        rig.run(0.6, 0.9);
        for (const double h : rig.fizz.levels()) {
            CHECK(h >= 0.0);
            CHECK(h <= FizzSim::kMaxHeight);
        }
        for (const FizzSim::Bubble& b : rig.fizz.bubbles()) {
            CHECK(b.y > -10.0);
            CHECK(b.lane >= 0);
            CHECK(b.lane < FizzSim::kLanes);
        }
        rig.run(0.6, kSilence);
    }
}

TEST_CASE("the finish rolls the levels from where they were into a wave", "[fizz]") {
    Rig rig;
    rig.run(1.0, kSpeech);
    const FizzSim::Levels before = rig.fizz.levels();
    rig.fizz.begin_work();
    // At morph 0 the glasses are where the recording left them, at 1 they are the wave.
    const FizzSim::Levels at_start = rig.fizz.work_levels(0.0, 0.7);
    for (int i = 0; i < FizzSim::kLanes; ++i) {
        CHECK(at_start[static_cast<std::size_t>(i)] == before[static_cast<std::size_t>(i)]);
    }
    const FizzSim::Levels wave = rig.fizz.work_levels(1.0, 0.7);
    for (const double h : wave) {
        CHECK(h >= 10.0);
        CHECK(h <= 30.0);
    }
    // The easing starts from the glasses' levels at begin_work, however often it is drawn.
    const FizzSim::Levels half = rig.fizz.work_levels(0.5, 0.7);
    for (int i = 0; i < FizzSim::kLanes; ++i) {
        const auto k = static_cast<std::size_t>(i);
        CHECK(half[k] == before[k] + (wave[k] - before[k]) * 0.5);
    }
}

TEST_CASE("bubbles fizz through the finish without lifting the levels", "[fizz]") {
    Rig rig;
    rig.run(1.0, kSpeech);
    rig.fizz.begin_work();
    rig.fizz.work_levels(1.0, 0.0);
    const FizzSim::Levels held = rig.fizz.levels();
    const FizzSim::Levels velocity = rig.fizz.velocity();
    for (int i = 0; i < 30; ++i) {
        rig.fizz.advance_work(1.0 / 30.0, 1.7);
    }
    CHECK(rig.fizz.levels() == held);
    CHECK(rig.fizz.velocity() == velocity);
    CHECK_FALSE(rig.fizz.bubbles().empty());
}
