// The golden vectors of ui_qt.overlays.live_recording (VoiceRibbon, its drawn shape,
// format_clock, particle_hue) and the port of the OpenWhisper ribbon test.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "overlay_test_support.h"
#include "uimath/live_label.h"
#include "uimath/loudness.h"
#include "uimath/voice_ribbon.h"

using Catch::Matchers::WithinAbs;
using fv::Json;
using fv::overlay_test::matches_result;
using fv::overlay_test::point_json;
using fv::overlay_test::points_json;
using fv::overlay_test::rgba_json;

namespace {

constexpr char kModule[] = "ui_qt.overlays.live_recording";

// A frame of the case files: the levels heard, then the frame's length.
void play(fv::VoiceRibbon& ribbon, const Json& frames, Json* meter = nullptr) {
    for (const Json& frame : frames) {
        for (const double level : fv::golden::decode<std::vector<double>>(frame.at(0))) {
            ribbon.hear(level);
        }
        ribbon.advance(fv::golden::decode<double>(frame.at(1)));
        if (meter != nullptr) {
            meter->push_back(Json::array({ribbon.loudness(), ribbon.shown()}));
        }
    }
}

Json stops_json(const std::vector<fv::GradientStop>& stops) {
    Json out = Json::array();
    for (const auto& stop : stops) {
        out.push_back(Json::array({stop.position, rgba_json(stop.color)}));
    }
    return out;
}

} // namespace

TEST_CASE("VoiceRibbon replays the golden frames", "[golden][voice_ribbon]") {
    fv::golden::for_each(kModule, "VoiceRibbon", [](const fv::golden::Case& c) {
        const double width = c.arg<double>(1);
        const auto xs = c.arg<std::vector<double>>(2);
        const double baseline = c.arg<double>(3);
        fv::VoiceRibbon ribbon;
        Json frames = Json::array();
        play(ribbon, c.args().at(0), &frames);
        Json heights = Json::array();
        Json crests = Json::array();
        for (const double x : xs) {
            heights.push_back(ribbon.height_at(x, width));
            crests.push_back(ribbon.crest(x, width, baseline));
        }
        const Json result{{"frames", frames},
                          {"history", ribbon.history()},
                          {"height_at", heights},
                          {"crest", crests}};
        CHECK(matches_result(c, result));
    });
}

TEST_CASE("VoiceRibbon draws the shape the golden painter recorded", "[golden][voice_ribbon]") {
    fv::golden::for_each(kModule, "VoiceRibbon.draw", [](const fv::golden::Case& c) {
        fv::VoiceRibbon ribbon;
        play(ribbon, c.args().at(0));
        const double width = c.arg<double>(1);
        const auto shape = ribbon.shape(width, c.arg<double>(2), c.arg<double>(3), c.arg<double>(4),
                                        c.arg<std::string>(5) == "dark");
        if (!shape) {
            CHECK(c.raw().at("result").is_null());
            return;
        }
        const Json result{
            {"top", points_json(shape->top)},
            {"area_first", point_json({shape->top.front().x, shape->bottom_y})},
            {"area_last", point_json({shape->top.back().x, shape->bottom_y})},
            {"gradient_start", point_json({shape->gradient_left, 0.0})},
            {"gradient_end", point_json({shape->gradient_right, 0.0})},
            {"fill", stops_json(shape->fill)},
            {"edge", stops_json(shape->edge)},
            {"edge_start", point_json({shape->gradient_left, 0.0})},
            {"edge_end", point_json({shape->gradient_right, 0.0})},
            {"pen_width", shape->edge_pen_width},
        };
        CHECK(matches_result(c, result));
    });
}

TEST_CASE("format_clock replays the golden vectors", "[golden][voice_ribbon]") {
    fv::golden::for_each(kModule, "format_clock", [](const fv::golden::Case& c) {
        CHECK(fv::format_clock(c.arg<double>(0)) == c.result<std::string>());
    });
}

TEST_CASE("particle_hue replays the golden vectors", "[golden][voice_ribbon]") {
    fv::golden::for_each(kModule, "particle_hue", [](const fv::golden::Case& c) {
        const double got = fv::particle_hue(c.arg<double>(0), c.arg<double>(1));
        CHECK(matches_result(c, Json(got)));
    });
}

// from tests/test_recording_look.py::test_the_ribbon_rises_with_speech_and_lies_flat_in_silence
TEST_CASE("the ribbon rises with speech and lies flat in silence", "[test_recording_look]") {
    constexpr double kSpeech = 0.06;
    constexpr double kSilence = 0.002;
    fv::VoiceRibbon ribbon;
    for (int i = 0; i < 60; ++i) {
        ribbon.hear(kSilence);
        ribbon.advance(1.0 / 30.0);
    }
    double flat = 0.0;
    for (int x = 14; x < 287; x += 4) {
        flat = std::max(flat, ribbon.height_at(x, 300));
    }
    for (int i = 0; i < 30; ++i) {
        ribbon.hear(kSpeech);
        ribbon.advance(1.0 / 30.0);
    }
    const double newest = ribbon.height_at(285, 300);
    CHECK(flat < 0.15);
    CHECK(newest > 0.5);
    CHECK(0.5 > 3 * flat);
    CHECK(ribbon.crest(285, 300, 50) < 50 - fv::kRibbonRise * 0.5);
    ribbon.reset();
    CHECK(ribbon.height_at(285, 300) == 0.0);
}

TEST_CASE("the meter rises at once and falls on a 60 ms time constant", "[voice_ribbon]") {
    fv::VoiceRibbon ribbon;
    ribbon.hear(0.1);
    ribbon.advance(0.001);
    const double peak = ribbon.shown();
    CHECK(peak == fv::level_to_height(0.1));
    ribbon.hear(0.0);
    ribbon.advance(0.06);
    CHECK_THAT(ribbon.shown(), WithinAbs(peak * std::exp(-1.0), 1e-12));
}

TEST_CASE("several levels between frames count as the loudest", "[voice_ribbon]") {
    fv::VoiceRibbon ribbon;
    ribbon.hear(0.1);
    ribbon.hear(0.001);
    ribbon.advance(0.001);
    CHECK(ribbon.shown() == fv::level_to_height(0.1));
}

TEST_CASE("the history holds 100 slots, one per 22 ms, newest last", "[voice_ribbon]") {
    fv::VoiceRibbon ribbon;
    CHECK(ribbon.history().size() == 100);
    ribbon.hear(0.1);
    for (int i = 0; i < 10; ++i) {
        ribbon.advance(0.0221);
    }
    const auto history = ribbon.history();
    CHECK(history.size() == 100);
    CHECK(history.front() == 0.0);
    CHECK(history.back() > 0.5);
    CHECK(history[89] == 0.0);
}

TEST_CASE("height_at interpolates between slots over the 14 px margins", "[voice_ribbon]") {
    fv::VoiceRibbon ribbon;
    ribbon.hear(1.0);
    ribbon.advance(2.2);
    // Left of the margin and right of it read the first and last slot.
    CHECK(ribbon.height_at(-50, 300) == ribbon.history().front());
    CHECK(ribbon.height_at(999, 300) == ribbon.history().back());
    // Too narrow for any span: still defined.
    CHECK(ribbon.height_at(5, 10) == ribbon.history().front());
}

TEST_CASE("the ribbon's shape is absent until it grows in", "[voice_ribbon]") {
    fv::VoiceRibbon ribbon;
    CHECK(!ribbon.shape(300, 50, 0.0, 0.0, true).has_value());
    const auto shape = ribbon.shape(300, 50, 0.0, 1.0, true);
    REQUIRE(shape.has_value());
    CHECK(shape->top.size() == 81);
    CHECK(shape->fill.size() == 4);
    CHECK(shape->edge.size() == 4);
}

TEST_CASE("format_clock reads m:ss", "[voice_ribbon]") {
    CHECK(fv::format_clock(0.0) == "0:00");
    CHECK(fv::format_clock(4.9) == "0:04");
    CHECK(fv::format_clock(60.0) == "1:00");
    CHECK(fv::format_clock(3661.0) == "61:01");
    CHECK(fv::format_clock(-3.0) == "0:00");
}
