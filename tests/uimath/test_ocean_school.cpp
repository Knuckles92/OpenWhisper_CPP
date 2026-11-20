// The golden vectors of ui_qt.overlays.ocean_looks (the school), and the ports of the
// OpenWhisper tests that run on it: the swell heaves with the voice, the bait ball follows
// phrases. Python's random.Random(5) stream is replayed, so a run is a vector draw for draw.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "overlay_test_support.h"
#include "uimath/live_label.h"
#include "uimath/loudness.h"
#include "uimath/ocean_school.h"
#include "uimath/voice_ribbon.h"

using fv::Json;
using fv::SchoolKind;
using fv::SchoolSim;
using fv::overlay_test::matches_result;

namespace {

constexpr char kModule[] = "ui_qt.overlays.ocean_looks";
constexpr double kSpeech = 0.06; // a word on a desk microphone, as raw RMS
constexpr double kSilence = 0.002;

SchoolKind kind_of(const std::string& name) {
    static const std::map<std::string, SchoolKind> kinds = {{"baitball", SchoolKind::BaitBall},
                                                            {"fish", SchoolKind::RealFish},
                                                            {"glow", SchoolKind::DeepGlow}};
    return kinds.at(name);
}

Json fish_json(const SchoolSim& school) {
    Json out = Json::array();
    for (const fv::Fish& f : school.fish()) {
        out.push_back(Json::array({f.x, f.y, f.vx, f.vy, f.r, f.hue, f.phase}));
    }
    return out;
}

double mean_y(const SchoolSim& school) {
    double sum = 0.0;
    for (const fv::Fish& f : school.fish()) {
        sum += f.y;
    }
    return sum / static_cast<double>(school.fish().size());
}

double row_max(const SchoolSim& school, int layer) {
    const auto& row = school.displacement()[static_cast<std::size_t>(layer)];
    return *std::max_element(row.begin(), row.end());
}

double row_min(const SchoolSim& school, int layer) {
    const auto& row = school.displacement()[static_cast<std::size_t>(layer)];
    return *std::min_element(row.begin(), row.end());
}

Json sample_json(const SchoolSim& school) {
    std::vector<double> ys;
    for (const fv::Fish& f : school.fish()) {
        ys.push_back(f.y);
    }
    std::sort(ys.begin(), ys.end());
    return Json::array({school.level(), mean_y(school), row_max(school, 0), row_min(school, 0),
                        school.time(), school.morph(), school.voice(), ys.front(), ys.back()});
}

Json layers_json(const SchoolSim::Layers& layers) {
    Json out = Json::array();
    for (const auto& row : layers) {
        out.push_back(Json(std::vector<double>(row.begin(), row.end())));
    }
    return out;
}

Json state_json(const SchoolSim& school) {
    Json fish = Json::array();
    Json trail_length = Json::array();
    Json trails = Json::array();
    for (std::size_t i = 0; i < school.fish().size(); ++i) {
        const fv::Fish& f = school.fish()[i];
        fish.push_back(Json::array({f.x, f.y, f.vx, f.vy}));
        trail_length.push_back(f.trail.size());
        if (i < 3) {
            Json trail = Json::array();
            for (const fv::PointD& p : f.trail) {
                trail.push_back(Json::array({p.x, p.y}));
            }
            trails.push_back(trail);
        }
    }
    Json y_of = Json::array();
    for (int layer = 0; layer < SchoolSim::kLayers; ++layer) {
        Json row = Json::array();
        for (const int p : {0, 1, 11, 24, 36, 47}) {
            row.push_back(school.y_of(layer, p));
        }
        y_of.push_back(row);
    }
    return Json{{"fish", fish},
                {"trail", trails},
                {"trail_length", trail_length},
                {"u", layers_json(school.displacement())},
                {"uv", layers_json(school.velocity())},
                {"t", school.time()},
                {"level", school.level()},
                {"working", school.working()},
                {"work_t", school.work_time()},
                {"rate", school.work_rate()},
                {"y_of", y_of},
                {"first", Json::array({school.fish()[0].x, school.fish()[0].y})}};
}

SchoolSim::Layers layers_of(const Json& json) {
    SchoolSim::Layers layers{};
    for (std::size_t layer = 0; layer < layers.size(); ++layer) {
        for (std::size_t p = 0; p < layers[layer].size(); ++p) {
            layers[layer][p] = fv::golden::decode<double>(json.at(layer).at(p));
        }
    }
    return layers;
}

// Where two JSON values first differ beyond tolerance, as a path, for a readable failure.
std::string first_difference(const Json& expected, const Json& actual, double tolerance,
                             const std::string& path = "") {
    if (expected.is_array() && actual.is_array() && expected.size() == actual.size()) {
        for (std::size_t i = 0; i < expected.size(); ++i) {
            const std::string where = first_difference(expected[i], actual[i], tolerance,
                                                       path + "[" + std::to_string(i) + "]");
            if (!where.empty()) {
                return where;
            }
        }
        return "";
    }
    if (expected.is_object() && actual.is_object()) {
        for (const auto& [key, value] : expected.items()) {
            if (!actual.contains(key)) {
                return path + "." + key + " missing";
            }
            const std::string where =
                first_difference(value, actual.at(key), tolerance, path + "." + key);
            if (!where.empty()) {
                return where;
            }
        }
        return "";
    }
    if (fv::golden::matches(expected, actual, tolerance)) {
        return "";
    }
    return path + ": expected " + expected.dump() + ", got " + actual.dump();
}

// The overlay's frame loop on a school: the ribbon hears the level, then both advance a frame.
struct Rig {
    explicit Rig(SchoolKind kind) : school(kind) {}

    void run(double seconds, double level) {
        const double end = time + seconds;
        while (time < end) {
            ribbon.hear(level);
            ribbon.advance(kFrame);
            school.advance(kFrame, ribbon.shown());
            time += kFrame;
        }
    }

    static constexpr double kFrame = 1.0 / 30.0;
    fv::VoiceRibbon ribbon;
    SchoolSim school;
    double time = 0.0;
};

} // namespace

TEST_CASE("python_random_engine replays Python's random streams", "[golden][ocean_school]") {
    fv::golden::for_each(kModule, "python_random", [](const fv::golden::Case& c) {
        const auto seed = c.arg<std::uint32_t>(0);
        const auto count = c.arg<int>(1);
        const auto bound = c.arg<int>(2);
        std::mt19937 unit = fv::python_random_engine(seed);
        std::mt19937 uniform = fv::python_random_engine(seed);
        std::mt19937 below = fv::python_random_engine(seed);
        Json u = Json::array();
        Json w = Json::array();
        Json b = Json::array();
        for (int i = 0; i < count; ++i) {
            u.push_back(fv::random_unit(unit));
            w.push_back(fv::random_uniform(uniform, -25.0, 25.0));
            b.push_back(fv::random_below(below, bound));
        }
        CHECK(c.raw().at("result").at("unit") == u);
        CHECK(c.raw().at("result").at("uniform") == w);
        CHECK(c.raw().at("result").at("below") == b);
    });
}

TEST_CASE("random_below refuses an empty range as randrange does", "[ocean_school]") {
    std::mt19937 rng = fv::python_random_engine(5);
    CHECK_THROWS_AS(fv::random_below(rng, 0), std::invalid_argument);
    CHECK_THROWS_AS(fv::random_below(rng, -3), std::invalid_argument);
    CHECK(fv::random_below(rng, 1) == 0);
}

TEST_CASE("the school's starting fish are Python's", "[golden][ocean_school]") {
    fv::golden::for_each(kModule, "School.reset", [](const fv::golden::Case& c) {
        SchoolSim school(kind_of(c.arg<std::string>(0)), c.arg<double>(1));
        CHECK(static_cast<int>(school.fish().size()) ==
              c.raw().at("result").at("count").get<int>());
        CHECK(
            matches_result(c, Json{{"count", school.fish().size()}, {"fish", fish_json(school)}}));
    });
}

TEST_CASE("a scripted run of the school replays the golden frames", "[golden][ocean_school]") {
    fv::golden::for_each(kModule, "School.run", [](const fv::golden::Case& c) {
        SchoolSim school(kind_of(c.arg<std::string>(0)), c.arg<double>(1));
        Json samples = Json::array();
        for (const Json& frame : c.args().at(2)) {
            school.advance(fv::golden::decode<double>(frame.at(1)),
                           fv::golden::decode<double>(frame.at(0)));
            samples.push_back(sample_json(school));
        }
        Json work_samples = Json::array();
        const Json& work = c.args().at(3);
        if (!work.empty()) {
            school.begin_work();
            for (const Json& frame : work) {
                school.advance_work(fv::golden::decode<double>(frame.at(0)),
                                    fv::golden::decode<double>(frame.at(1)));
                work_samples.push_back(sample_json(school));
            }
        }
        const Json actual{
            {"samples", samples}, {"work_samples", work_samples}, {"state", state_json(school)}};
        INFO(first_difference(c.raw().at("result"), actual, c.float_tolerance()));
        CHECK(matches_result(c, actual));
    });
}

TEST_CASE("the swell's springs step as Python's do", "[golden][ocean_school]") {
    fv::golden::for_each(kModule, "School._roll_springs", [](const fv::golden::Case& c) {
        SchoolSim::Layers u = layers_of(c.args().at(0));
        SchoolSim::Layers uv = layers_of(c.args().at(1));
        SchoolSim::roll_springs(u, uv, c.arg<double>(2));
        CHECK(matches_result(c, Json{{"u", layers_json(u)}, {"uv", layers_json(uv)}}));
    });
}

TEST_CASE("the school's colours replay the golden vectors on both themes",
          "[golden][ocean_school]") {
    fv::golden::for_each(kModule, "School.colors", [](const fv::golden::Case& c) {
        const SchoolKind kind = kind_of(c.arg<std::string>(0));
        const bool dark = c.arg<std::string>(1) == "dark";
        const double t = c.arg<double>(2);
        const double hue = fv::particle_hue(c.arg<double>(3), 50.0);
        const double alpha = c.arg<double>(4);
        Json ocean = Json::array();
        for (int layer = 0; layer < SchoolSim::kLayers; ++layer) {
            const fv::Rgba color =
                SchoolSim::ocean_color_at(t, layer, alpha, c.arg<double>(5), dark);
            ocean.push_back(fv::overlay_test::rgba_json(color));
        }
        Json fish = Json::array();
        const SchoolSim fresh(kind);
        for (std::size_t i = 0; i < 8; ++i) {
            fish.push_back(fv::overlay_test::rgba_json(
                SchoolSim::fish_color_at(kind, t, fresh.fish()[i], alpha, hue, dark)));
        }
        CHECK(c.raw().at("result").at("ocean") == ocean);
        CHECK(c.raw().at("result").at("fish") == fish);
    });
}

TEST_CASE("the school's constants are Python's", "[golden][ocean_school]") {
    fv::golden::for_each(kModule, "tables", [](const fv::golden::Case& c) {
        const Json& school = c.raw().at("result").at("school");
        CHECK(school.at("BASE").get<double>() == SchoolSim::kBase);
        CHECK(school.at("TOP").get<double>() == SchoolSim::kTop);
        CHECK(school.at("LAYERS").get<int>() == SchoolSim::kLayers);
        CHECK(school.at("POINTS").get<int>() == SchoolSim::kPoints);
        CHECK(school.at("BOTTOM").get<double>() == SchoolSim::kBottom);
        CHECK(school.at("GAP").get<double>() == SchoolSim::kGap);
        CHECK(school.at("GAP_MIN").get<double>() == SchoolSim::kGapMin);
        CHECK(school.at("WATER_HUE").get<double>() == SchoolSim::kWaterHue);
        const Json& kinds = c.raw().at("result").at("kinds");
        CHECK(SchoolSim(SchoolKind::BaitBall).fish().size() ==
              kinds.at("baitball").at(0).get<std::size_t>());
        CHECK(SchoolSim(SchoolKind::RealFish).fish().size() ==
              kinds.at("fish").at(0).get<std::size_t>());
        CHECK(SchoolSim(SchoolKind::DeepGlow).fish().size() ==
              kinds.at("glow").at(0).get<std::size_t>());
        CHECK(SchoolSim::kBaitBallSquash == kinds.at("baitball").at(2).get<double>());
        CHECK(SchoolSim::kDeepGlowTrail == kinds.at("glow").at(1).get<int>());
        CHECK(SchoolSim::kDeepGlowPingS == kinds.at("glow").at(2).get<double>());
        CHECK(school.at("RANGE").get<double>() == 26.0);
        CHECK(school.at("SWELL").get<double>() == 1.8);
    });
}

// from tests/test_recording_look.py::test_the_school_heaves_the_swell_with_the_voice
TEST_CASE("the school heaves the swell with the voice", "[test_recording_look][ocean_school]") {
    const SchoolKind kind =
        GENERATE(SchoolKind::BaitBall, SchoolKind::RealFish, SchoolKind::DeepGlow);
    Rig rig(kind);
    rig.run(1.5, kSilence);
    const double calm = row_max(rig.school, 0);
    rig.run(1.5, kSpeech);
    CHECK(row_max(rig.school, 0) > calm + 3); // the school surges up and pushes the bottom swell
    // Every layer stays above the one beneath it.
    for (int p = 0; p < SchoolSim::kPoints; ++p) {
        for (int layer = 1; layer < SchoolSim::kLayers; ++layer) {
            CHECK(rig.school.y_of(layer, p) <=
                  rig.school.y_of(layer - 1, p) - SchoolSim::kGapMin + 0.5);
        }
    }
}

// from tests/test_recording_look.py::test_the_bait_ball_rises_while_talking_and_sinks_in_pauses
TEST_CASE("the bait ball rises while talking and sinks in pauses",
          "[test_recording_look][ocean_school]") {
    // Real speech is words with gaps; the ball must follow phrases, not average them away.
    Rig rig(SchoolKind::BaitBall);
    auto height = [&] {
        return SchoolSim::kBase - mean_y(rig.school);
    };
    rig.run(1.0, kSilence);
    std::vector<double> talking;
    std::vector<double> resting;
    for (int phrase = 0; phrase < 2; ++phrase) {
        for (int word = 0; word < 6; ++word) { // a phrase: words with short gaps
            rig.run(0.18, kSpeech);
            rig.run(0.07, kSilence);
            if (word >= 2) {
                talking.push_back(height());
            }
        }
        for (int beat = 0; beat < 9; ++beat) { // a pause
            rig.run(0.1, kSilence);
            if (beat >= 5) {
                resting.push_back(height());
            }
        }
    }
    auto average = [](const std::vector<double>& v) {
        double sum = 0.0;
        for (const double x : v) {
            sum += x;
        }
        return sum / static_cast<double>(v.size());
    };
    const double swing = average(talking) - average(resting);
    INFO("talking should lift the ball well above where it rests (swing " << swing << " px)");
    CHECK(swing > 12);
}

TEST_CASE("the stack never sinks below its floor or climbs past its ceiling", "[ocean_school]") {
    const SchoolKind kind =
        GENERATE(SchoolKind::BaitBall, SchoolKind::RealFish, SchoolKind::DeepGlow);
    Rig rig(kind);
    for (int burst = 0; burst < 6; ++burst) {
        rig.run(0.5, 0.15);
        rig.run(0.5, kSilence);
        for (int layer = 0; layer < SchoolSim::kLayers; ++layer) {
            // u >= -4, and each layer under its ceiling.
            CHECK(row_min(rig.school, layer) >= -4.0);
            for (int p = 0; p < SchoolSim::kPoints; ++p) {
                CHECK(rig.school.y_of(layer, p) >=
                      SchoolSim::kTop + (SchoolSim::kLayers - 1 - layer) * SchoolSim::kGapMin -
                          1e-9);
            }
        }
    }
    for (const fv::Fish& f : rig.school.fish()) {
        CHECK(f.y <= SchoolSim::kBase - f.r + 1e-9);
        CHECK(f.y >= SchoolSim::kTop - 1e-9);
        CHECK(f.x >= fv::kRibbonEdge - 1e-9);
        CHECK(f.x <= 300.0 - fv::kRibbonEdge + 1e-9);
    }
}

TEST_CASE("a new recording rewinds the school to the same starting fish", "[ocean_school]") {
    SchoolSim school(SchoolKind::RealFish);
    const Json first = fish_json(school);
    for (int i = 0; i < 40; ++i) {
        school.advance(1.0 / 30.0, 0.8);
    }
    CHECK(fish_json(school) != first);
    school.reset();
    CHECK(fish_json(school) == first);
    CHECK(school.level() == 0.0);
    CHECK(school.time() == 0.0);
    CHECK_FALSE(school.working());
}

TEST_CASE("a frame of no length changes nothing", "[ocean_school]") {
    SchoolSim school(SchoolKind::DeepGlow);
    school.advance(1.0 / 30.0, 0.5);
    const Json before = state_json(school);
    school.advance(0.0, 0.9);
    school.advance(-0.1, 0.9);
    CHECK(state_json(school) == before);
}

TEST_CASE("the work morphs in and the voice eases out", "[ocean_school]") {
    SchoolSim school(SchoolKind::BaitBall);
    for (int i = 0; i < 30; ++i) {
        school.advance(1.0 / 30.0, 0.9);
    }
    const double level = school.level();
    CHECK(level > 0.3);
    CHECK(school.voice() == level);
    school.begin_work();
    CHECK(school.morph() == 0.0);
    CHECK(school.voice() == level);
    school.advance_work(0.25, 1.0);
    CHECK(school.morph() > 0.0);
    CHECK(school.morph() < 1.0);
    CHECK(school.voice() < level);
    school.advance_work(0.5, 1.7);
    CHECK(school.morph() == 1.0);
    CHECK(school.voice() == 0.0);
    CHECK(school.work_rate() == 1.7);
}

TEST_CASE("deep-sea glow's fish keep a trail of seven positions", "[ocean_school]") {
    SchoolSim glow(SchoolKind::DeepGlow);
    SchoolSim fish(SchoolKind::RealFish);
    for (int i = 0; i < 20; ++i) {
        glow.advance(1.0 / 30.0, 0.5);
        fish.advance(1.0 / 30.0, 0.5);
    }
    for (const fv::Fish& f : glow.fish()) {
        CHECK(f.trail.size() == 7);
    }
    for (const fv::Fish& f : fish.fish()) {
        CHECK(f.trail.empty());
    }
}
