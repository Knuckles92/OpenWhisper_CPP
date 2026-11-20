// The golden vectors of ui_qt.overlays.recording_looks (look names, smooth, hue_color).
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>

#include "overlay_test_support.h"
#include "uimath/overlay_look.h"

using fv::overlay_test::matches_result;
using fv::overlay_test::rgba_json;

namespace {

constexpr char kModule[] = "ui_qt.overlays.recording_looks";

} // namespace

TEST_CASE("normalize_look replays the golden vectors", "[golden][overlay_look]") {
    fv::golden::for_each(kModule, "normalize_look", [](const fv::golden::Case& c) {
        const fv::Json& value = c.args().at(0);
        // A stored value that is not even text is no look: the default.
        const std::string got = value.is_string()
                                    ? std::string(fv::normalize_look(value.get<std::string>()))
                                    : std::string(fv::kDefaultLook);
        CHECK(got == c.result<std::string>());
    });
}

TEST_CASE("smooth replays the golden vectors", "[golden][overlay_look]") {
    fv::golden::for_each(kModule, "smooth", [](const fv::golden::Case& c) {
        CHECK(matches_result(c, fv::Json(fv::smooth(c.arg<double>(0)))));
    });
}

TEST_CASE("hue_color replays the golden vectors on both themes", "[golden][overlay_look]") {
    fv::golden::for_each(kModule, "hue_color", [](const fv::golden::Case& c) {
        const bool dark = c.arg<std::string>(0) == "dark";
        const double hue = c.arg<double>(1);
        const double alpha = c.arg_count() > 2 ? c.arg<double>(2) : 255.0;
        const bool bright = c.arg_count() > 3 ? c.arg<bool>(3) : true;
        CHECK(c.raw().at("result") == rgba_json(fv::hue_color(hue, alpha, bright, dark)));
    });
}

TEST_CASE("lerp_point replays the golden vectors", "[golden][overlay_look]") {
    fv::golden::for_each(kModule, "lerp_point", [](const fv::golden::Case& c) {
        const auto a = c.arg<std::vector<double>>(0);
        const auto b = c.arg<std::vector<double>>(1);
        const fv::PointD got =
            fv::lerp_point({a.at(0), a.at(1)}, {b.at(0), b.at(1)}, c.arg<double>(2));
        CHECK(matches_result(c, fv::overlay_test::point_json(got)));
    });
}

TEST_CASE("the look tables equal OpenWhisper's", "[golden][overlay_look]") {
    fv::golden::for_each(kModule, "tables", [](const fv::golden::Case& c) {
        const fv::Json& want = c.raw().at("result");
        fv::Json looks = fv::Json::array();
        for (const auto look : fv::kLooks) {
            looks.push_back(std::string(look));
        }
        CHECK(want.at("looks") == looks);
        CHECK(want.at("default").get<std::string>() == fv::kDefaultLook);
        CHECK(want.at("aliases").at("live").get<std::string>() == fv::kLookRibbon);
        CHECK(want.at("aliases").at("orb").get<std::string>() == fv::kLookPulse);
        CHECK(want.at("aliases").at("bubbles").get<std::string>() == fv::kLookBaitBall);
        CHECK(want.at("aliases").size() == 3);
        CHECK(want.at("center_y").get<double>() == fv::kCenterY);
        CHECK(want.at("morph_s").get<double>() == fv::kMorphS);
        CHECK(want.at("squash").get<std::string>() == fv::kSmallSquash);
        CHECK(want.at("shrink").get<std::string>() == fv::kSmallShrink);
        CHECK(want.at("fit").get<std::string>() == fv::kSmallFit);
    });
}

TEST_CASE("normalize_look keeps a current name and maps the old ones", "[overlay_look]") {
    for (const auto look : fv::kLooks) {
        CHECK(fv::normalize_look(look) == look);
    }
    CHECK(fv::normalize_look("live") == "ribbon");
    CHECK(fv::normalize_look("orb") == "pulse");
    CHECK(fv::normalize_look("bubbles") == "baitball");
    CHECK(fv::normalize_look("") == "bars");
    CHECK(fv::normalize_look("Ribbon") == "bars");
    CHECK(fv::normalize_look("nonsense") == "bars");
}

TEST_CASE("hue_color picks the theme on every call", "[overlay_look]") {
    const fv::Rgba dark = fv::hue_color(200.0, 255.0, true, true);
    const fv::Rgba light = fv::hue_color(200.0, 255.0, true, false);
    CHECK(!(dark == light));
    CHECK(fv::hue_color(200.0, 255.0, true, true) == dark);
    // The dimmer variant exists on the dark theme only.
    CHECK(!(fv::hue_color(200.0, 255.0, false, true) == dark));
    CHECK(fv::hue_color(200.0, 255.0, false, false) == light);
}

TEST_CASE("hue_degrees is Python's int(hue) % 360 for any finite hue", "[overlay_look]") {
    CHECK(fv::hue_degrees(0.0) == 0);
    CHECK(fv::hue_degrees(359.99) == 359);
    CHECK(fv::hue_degrees(-0.5) == 0);
    CHECK(fv::hue_degrees(-1.0) == 359);
    CHECK(fv::hue_degrees(-721.0) == 359);
    // Past the range of a 64-bit integer: 1e20 = 360 * 277777777777777777 + 280 exactly.
    CHECK(fv::hue_degrees(1e20) == 280);
    CHECK(fv::hue_degrees(-1e20) == 80);
    // Python raises here; a colour is still owed.
    CHECK(fv::hue_degrees(std::numeric_limits<double>::quiet_NaN()) == 0);
    CHECK(fv::hue_degrees(std::numeric_limits<double>::infinity()) == 0);
    CHECK(fv::hue_color(1e20, 255.0, true, true) == fv::hue_color(280.0, 255.0, true, true));
}
