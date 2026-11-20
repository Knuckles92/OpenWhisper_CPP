// The golden vectors of ui_qt.overlays.moments (glyph paths, tables) and the statistics of its
// confetti and bubbles, whose random stream std::mt19937 cannot reproduce.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <random>
#include <stdexcept>
#include <string>

#include "overlay_test_support.h"
#include "uimath/moments.h"

using Catch::Matchers::WithinAbs;
using fv::Json;
using fv::overlay_test::matches_result;
using fv::overlay_test::points_json;

namespace {

constexpr char kModule[] = "ui_qt.overlays.moments";

Json moment_json(const fv::Moment& m) {
    return Json{{"tone", std::string(m.tone)},
                {"partner", std::string(m.partner)},
                {"glyph", std::string(m.glyph)},
                {"lead_in", std::string(m.lead_in)},
                {"pop_at", m.pop_at},
                {"hues", Json::array({Json::array({m.hues[0].low, m.hues[0].high}),
                                      Json::array({m.hues[1].low, m.hues[1].high})})},
                {"glint", std::string(m.glint)},
                {"fall", m.fall},
                {"afterglow", m.afterglow}};
}

} // namespace

TEST_CASE("glyph_path replays the golden vertices and lengths", "[golden][moments]") {
    fv::golden::for_each(kModule, "glyph_path", [](const fv::golden::Case& c) {
        const auto center = c.arg<std::vector<double>>(1);
        const fv::GlyphPath path =
            fv::glyph_path(c.arg<std::string>(0), {center.at(0), center.at(1)}, c.arg<double>(2));
        Json strokes = Json::array();
        for (const auto& stroke : path.strokes) {
            strokes.push_back(points_json(stroke));
        }
        CHECK(matches_result(c, Json{{"strokes", strokes}, {"length", path.length()}}));
    });
}

TEST_CASE("the moment and glyph tables equal OpenWhisper's", "[golden][moments]") {
    fv::golden::for_each(kModule, "tables", [](const fv::golden::Case& c) {
        const Json& want = c.raw().at("result");

        const Json constants{{"RADIUS", fv::kMomentRadius}, {"POP_S", fv::kPopS},
                             {"RING_S", fv::kRingS},        {"GLYPH_DELAY_S", fv::kGlyphDelayS},
                             {"GLYPH_S", fv::kGlyphS},      {"LABEL_DELAY_S", fv::kLabelDelayS},
                             {"LABEL_S", fv::kLabelS},      {"GLINT_DELAY_S", fv::kGlintDelayS},
                             {"GLINT_S", fv::kGlintS},      {"FADE_S", fv::kFadeS}};
        CHECK(fv::golden::equal(want.at("constants"), constants));

        Json sparkles = Json::array();
        for (const auto& s : fv::kSparkles) {
            sparkles.push_back(Json::array({s.x_frac, s.y_frac, s.base_size}));
        }
        CHECK(fv::golden::equal(want.at("sparkles"), sparkles));

        Json afterglow = Json::array();
        for (const auto& s : fv::kAfterglow) {
            afterglow.push_back(Json::array({s.dx, s.dy, s.size, s.phase}));
        }
        CHECK(fv::golden::equal(want.at("afterglow"), afterglow));

        Json glyphs = Json::object();
        Json widths = Json::object();
        for (const char* name : {"check", "cross", "bang", "clip"}) {
            Json strokes = Json::array();
            for (const auto& stroke : fv::glyph(name).strokes) {
                strokes.push_back(points_json(stroke));
            }
            glyphs[name] = strokes;
            widths[name] = fv::glyph(name).pen_width;
        }
        CHECK(fv::golden::equal(want.at("glyphs"), glyphs));
        CHECK(fv::golden::equal(want.at("glyph_widths"), widths));

        Json moments = Json::object();
        std::vector<std::string> order;
        for (const auto& named : fv::moments()) {
            moments[std::string(named.key)] = moment_json(named.moment);
            order.emplace_back(named.key);
        }
        CHECK(fv::golden::equal(want.at("moments"), moments));
        CHECK(order == std::vector<std::string>{"done", "enabled", "disabled", "copied"});
        CHECK(fv::golden::equal(want.at("canceled"), moment_json(fv::canceled_moment())));
    });
}

TEST_CASE("glyph and moment lookups name what exists", "[moments]") {
    CHECK_THROWS_AS(fv::glyph("star"), std::out_of_range);
    CHECK(fv::find_moment("done") != nullptr);
    CHECK(fv::find_moment("canceled") == nullptr);
    CHECK(fv::find_moment("Done") == nullptr);
    CHECK(fv::find_moment("copied")->glyph == "clip");
}

TEST_CASE("a glyph is drawn stroke by stroke as its amount grows", "[moments]") {
    const fv::PointD center{100.0, 30.0};
    CHECK(fv::glyph_path("cross", center, 0.0).strokes.empty());
    const fv::GlyphPath half = fv::glyph_path("cross", center, 0.5);
    REQUIRE(half.strokes.size() == 1);
    CHECK(half.strokes[0].size() == 2);
    const fv::GlyphPath full = fv::glyph_path("cross", center, 1.0);
    CHECK(full.strokes.size() == 2);
    CHECK_THAT(full.length(), WithinAbs(2 * std::hypot(8.4, 8.4), 1e-9));
    CHECK_THAT(half.length(), WithinAbs(0.5 * full.length(), 1e-9));
}

TEST_CASE("random_unit stays in [0, 1) and is evenly spread", "[moments][statistical]") {
    std::mt19937 rng(42);
    double sum = 0.0;
    std::array<int, 4> quarters{};
    constexpr int kDraws = 100000;
    for (int i = 0; i < kDraws; ++i) {
        const double v = fv::random_unit(rng);
        REQUIRE(v >= 0.0);
        REQUIRE(v < 1.0);
        sum += v;
        ++quarters[static_cast<std::size_t>(v * 4)];
    }
    CHECK_THAT(sum / kDraws, WithinAbs(0.5, 0.01));
    for (const int count : quarters) {
        CHECK_THAT(static_cast<double>(count) / kDraws, WithinAbs(0.25, 0.01));
    }
    for (int i = 0; i < 1000; ++i) {
        const int v = fv::random_int(rng, 0, 3);
        REQUIRE(v >= 0);
        REQUIRE(v <= 3);
    }
    std::mt19937 again(42);
    std::mt19937 first(42);
    CHECK(fv::random_unit(again) == fv::random_unit(first));
}

TEST_CASE("a confetti burst fills a ring flattened to the pill", "[moments][statistical]") {
    std::mt19937 rng(7);
    const fv::Moment& moment = *fv::find_moment("done");
    const std::vector<fv::Particle> dots = fv::burst(moment, 150.0, 40.0, rng);
    REQUIRE(dots.size() == 40);
    for (std::size_t i = 0; i < dots.size(); ++i) {
        const fv::Particle& p = dots[i];
        CHECK(std::fabs(p.x - 150.0) <= 3.0);
        CHECK(std::fabs(p.y - 40.0) <= 3.0);
        CHECK(p.life == 1.0);
        CHECK(p.size >= 1.6);
        CHECK(p.size <= 3.0);
        // The hue alternates between the tone's range and the partner's.
        const fv::HueRange range = moment.hues[i % 2];
        CHECK(p.color_hue >= range.low);
        CHECK(p.color_hue <= range.high);
        // Speed 170-280 with the vertical part squashed to 35 %.
        const double along_x = std::fabs(p.vx);
        CHECK(along_x <= 280.0);
        CHECK(std::fabs(p.vy) <= 280.0 * 0.35 + 1e-9);
    }
    // The ring is spread all the way round.
    double mean_vx = 0.0;
    for (const auto& p : dots) {
        mean_vx += p.vx;
    }
    CHECK(std::fabs(mean_vx / 40.0) < 60.0);
}

TEST_CASE("confetti thins out and is gone in about a second", "[moments][statistical]") {
    std::mt19937 rng(11);
    std::vector<fv::Particle> dots = fv::burst(*fv::find_moment("enabled"), 0.0, 0.0, rng);
    constexpr double kDt = 1.0 / 30.0;
    int ticks = 0;
    while (!dots.empty() && ticks < 100) {
        const auto before = dots.size();
        fv::advance_confetti(dots, kDt, 0.0);
        CHECK(dots.size() <= before);
        ++ticks;
    }
    // life falls by dt * (0.6 + 0.5) a tick, from 1: 28 ticks to reach 0.
    CHECK(ticks == 28);
}

TEST_CASE("falling confetti gains downward speed", "[moments][statistical]") {
    std::mt19937 rng(3);
    std::vector<fv::Particle> dots = fv::burst(*fv::find_moment("disabled"), 0.0, 0.0, rng);
    const double vy_before = dots[0].vy;
    fv::advance_confetti(dots, 0.01, 260.0);
    // Gravity adds 260 * 0.01 before the damping of 0.92 ** (0.01 * 30) takes its share.
    CHECK_THAT(dots[0].vy, WithinAbs((vy_before + 2.6) * std::pow(0.92, 0.3), 1e-12));
}

TEST_CASE("a bubble burst fills the pill and outlives confetti", "[moments][statistical]") {
    std::mt19937 rng(5);
    const fv::Moment& moment = fv::canceled_moment();
    std::vector<fv::Particle> bubbles = fv::bubble_burst(moment, 150.0, 40.0, rng);
    REQUIRE(bubbles.size() == 72);
    for (std::size_t i = 0; i < bubbles.size(); ++i) {
        const fv::Particle& p = bubbles[i];
        CHECK(p.x == 150.0);
        CHECK(p.y == 40.0);
        CHECK(p.size >= 2.5);
        CHECK(p.size <= 5.0);
        const fv::HueRange range = moment.hues[i % 2];
        CHECK(p.color_hue >= range.low);
        CHECK(p.color_hue <= range.high);
        CHECK(std::hypot(p.vx, p.vy / 0.6) <= 360.0 + 1e-9);
    }
    constexpr double kDt = 1.0 / 30.0;
    int ticks = 0;
    while (!bubbles.empty() && ticks < 200) {
        fv::advance_bubbles(bubbles, kDt, rng);
        ++ticks;
    }
    // life falls by dt * (0.35 + 0.5) a tick: 36 ticks.
    CHECK(ticks == 36);
    CHECK(ticks > 28);
}
