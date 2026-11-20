// The golden vectors of ui_qt.waveform_styles.particle_style, the particle tests of
// tests/test_desktop_performance.py, and statistics for the parts that draw from the random
// stream (std::mt19937 cannot reproduce Python's).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "overlay_test_support.h"
#include "uimath/particles.h"
#include "uimath/voice_ribbon.h"

using Catch::Matchers::WithinAbs;
using fv::Json;
using fv::ParticleConfig;
using fv::ParticleStyle;
using fv::overlay_test::matches_result;
using fv::overlay_test::particle_json;
using fv::overlay_test::rgba_json;

namespace {

constexpr char kModule[] = "ui_qt.waveform_styles.particle_style";

ParticleConfig config_from(const Json& object) {
    ParticleConfig config;
    for (const auto& [key, value] : object.items()) {
        if (key == "max_particles") {
            config.max_particles = value.get<int>();
        } else if (key == "emission_rate") {
            config.emission_rate = value.get<double>();
        } else if (key == "gravity") {
            config.gravity = value.get<double>();
        } else if (key == "damping") {
            config.damping = value.get<double>();
        } else if (key == "wind_strength") {
            config.wind_strength = value.get<double>();
        } else if (key == "audio_response") {
            config.audio_response = value.get<double>();
        } else if (key == "glow_effect") {
            config.glow_effect = value.get<bool>();
        } else if (key == "turbulence_strength") {
            config.turbulence_strength = value.get<double>();
        } else if (key == "color_shift_speed") {
            config.color_shift_speed = value.get<double>();
        } else {
            FAIL("unknown particle config key " << key);
        }
    }
    return config;
}

fv::Particle particle_of(double x, double y, double vx, double vy, double life, double hue) {
    fv::Particle p;
    p.x = x;
    p.y = y;
    p.vx = vx;
    p.vy = vy;
    p.life = life;
    p.color_hue = hue;
    return p;
}

// What painting reads from a style: everything it exposes, through its const accessors, with
// each particle's colours on both themes.
void paint(const ParticleStyle& style) {
    for (const fv::Particle& p : style.particles()) {
        for (const bool dark : {true, false}) {
            (void)p.color(dark);
            (void)p.fading_color(dark);
        }
    }
    (void)style.animation_time();
    (void)style.audio_levels();
    (void)style.hue();
}

} // namespace

TEST_CASE("Particle::update replays the golden trajectories", "[golden][particles]") {
    fv::golden::for_each(kModule, "Particle.update", [](const fv::golden::Case& c) {
        fv::Particle p = particle_of(c.arg<double>(0), c.arg<double>(1), c.arg<double>(2),
                                     c.arg<double>(3), c.arg<double>(4), 0.0);
        Json trajectory = Json::array();
        for (const Json& step : c.args().at(5)) {
            const bool alive = p.update(fv::golden::decode<double>(step.at(0)),
                                        fv::golden::decode<double>(step.at(1)),
                                        fv::golden::decode<double>(step.at(2)));
            trajectory.push_back(Json::array({alive, p.x, p.y, p.vx, p.vy, p.life}));
        }
        CHECK(matches_result(c, trajectory));
    });
}

TEST_CASE("Particle::color replays the golden colours", "[golden][particles]") {
    fv::golden::for_each(kModule, "Particle.get_qcolor", [](const fv::golden::Case& c) {
        const fv::Particle p = particle_of(0, 0, 0, 0, c.arg<double>(2), c.arg<double>(1));
        std::optional<double> base;
        if (c.arg_count() > 3) {
            base = c.arg<double>(3);
        }
        CHECK(c.raw().at("result") == rgba_json(p.color(c.arg<std::string>(0) == "dark", base)));
    });
}

TEST_CASE("Particle::fading_color replays the golden colours", "[golden][particles]") {
    fv::golden::for_each(kModule, "Particle.get_fading_color", [](const fv::golden::Case& c) {
        const fv::Particle p = particle_of(0, 0, 0, 0, c.arg<double>(2), c.arg<double>(1));
        CHECK(c.raw().at("result") == rgba_json(p.fading_color(c.arg<std::string>(0) == "dark")));
    });
}

TEST_CASE("the processing vortex replays the golden simulation", "[golden][particles]") {
    fv::golden::for_each(kModule, "ParticleStyle.vortex", [](const fv::golden::Case& c) {
        ParticleStyle style(c.arg<int>(0), c.arg<int>(1), config_from(c.args().at(2)),
                            std::mt19937(1));
        Json ticks = Json::array();
        for (const Json& tick : c.args().at(3)) {
            style.advance(tick.at(0).get<std::string>(), fv::golden::decode<double>(tick.at(1)));
            ticks.push_back(Json::array(
                {style.animation_time(), static_cast<std::int64_t>(style.particles().size())}));
        }
        Json particles = Json::array();
        for (const fv::Particle& p : style.particles()) {
            particles.push_back(particle_json(p));
        }
        CHECK(matches_result(c, Json{{"ticks", ticks}, {"particles", particles}}));
    });
}

// from tests/test_desktop_performance.py::test_particle_paints_do_not_advance_any_simulation_state
// The painting itself is the overlay's; what it reads must leave the simulation as it was.
TEST_CASE("particle paints do not advance any simulation state", "[test_desktop_performance]") {
    for (const char* state : {"recording", "processing", "transcribing"}) {
        ParticleStyle style(320, 100, ParticleConfig{}, std::mt19937(1));
        style.advance(state, 1.0 / 30.0);
        REQUIRE(!style.particles().empty());
        const auto before = style.particles();
        const double time_before = style.animation_time();
        for (int i = 0; i < 3; ++i) {
            paint(style);
        }
        CHECK(style.particles() == before);
        CHECK(style.animation_time() == time_before);
    }
}

// from tests/test_desktop_performance.py::
//      test_equal_elapsed_particle_ticks_match_across_frame_rates_and_clamp_stalls
TEST_CASE("equal elapsed particle ticks match across frame rates and clamp stalls",
          "[test_desktop_performance]") {
    std::vector<std::vector<fv::Particle>> outcomes;
    ParticleConfig config;
    config.max_particles = 1000;
    std::optional<ParticleStyle> last;
    for (const int fps : {30, 60}) {
        last.emplace(320, 100, config, std::mt19937(123));
        last->update_audio_levels({0.5});
        for (int i = 0; i < fps; ++i) {
            last->advance("recording", 1.0 / fps);
        }
        outcomes.push_back(last->particles());
    }
    CHECK(!outcomes[0].empty());
    CHECK(outcomes[0] == outcomes[1]);
    const double before = last->animation_time();
    last->advance("recording", 100.0);
    CHECK_THAT(last->animation_time() - before, WithinAbs(0.1, 1e-9));
}

TEST_CASE("a recording at rest emits the configured rate", "[particles][statistical]") {
    ParticleConfig config = fv::overlay_particle_config();
    ParticleStyle style(300, 80, config, std::mt19937(9));
    for (int i = 0; i < 30; ++i) {
        style.advance("recording", 1.0 / 30.0);
    }
    // 30 a second for one second; nothing has aged out yet (life lasts two seconds).
    CHECK(style.particles().size() == 30);
    for (const auto& p : style.particles()) {
        CHECK(std::isfinite(p.x));
        CHECK(std::isfinite(p.y));
        // The floor bounce pins y to 55 and the same step may move it a little.
        CHECK(p.y <= 80 - 25 + 1.0);
        CHECK(p.life > 0.0);
        CHECK(p.life <= 1.0);
        CHECK(p.size >= 1.5);
        CHECK(p.size <= 4.0);
    }
}

TEST_CASE("loud audio multiplies the emission by 1 + energy * audio_response",
          "[particles][statistical]") {
    ParticleConfig config = fv::overlay_particle_config();
    config.max_particles = 1000;
    config.audio_response = fv::kLiveAudioResponse;
    ParticleStyle style(300, 80, config, std::mt19937(9));
    style.update_audio_levels({0.5, 0.5});
    for (int i = 0; i < 30; ++i) {
        style.advance("recording", 1.0 / 30.0);
    }
    // 30 * (1 + 0.5 * 2.6) = 69 a second.
    CHECK(style.particles().size() == 69);
}

TEST_CASE("recording particles launch from the emitter's line", "[particles][statistical]") {
    ParticleConfig config = fv::overlay_particle_config();
    ParticleStyle style(300, 80, config, std::mt19937(2));
    style.emitter = [](double x) {
        return 10.0 + x * 0.1;
    };
    style.advance("recording", 0.1);
    REQUIRE(style.particles().size() == 3);
    for (const auto& p : style.particles()) {
        // Launched at 10 + x/10 (x in 20-280) and moved at most a few steps since; vy is -80 to
        // -40 (more with the drift's turbulence) over 1/120 s steps.
        CHECK(p.y >= 10.0 + 20 * 0.1 - 12.0);
        CHECK(p.y <= 10.0 + 280 * 0.1 + 12.0);
        CHECK(p.x >= 20.0 - 12.0);
        CHECK(p.x <= 280.0 + 12.0);
    }
}

TEST_CASE("the particle count never passes the maximum", "[particles][statistical]") {
    ParticleConfig config;
    config.max_particles = 10;
    config.emission_rate = 2000.0;
    ParticleStyle style(300, 80, config, std::mt19937(4));
    for (int i = 0; i < 20; ++i) {
        style.advance("recording", 1.0 / 30.0);
        CHECK(style.particles().size() <= 10);
    }
    CHECK(style.particles().size() == 10);
}

TEST_CASE("transcribing particles start outside and converge on the centre",
          "[particles][statistical]") {
    ParticleStyle style(300, 80, ParticleConfig{}, std::mt19937(8));
    style.advance("transcribing", 0.1);
    REQUIRE(!style.particles().empty());
    // 90 a second for a tenth of a second.
    CHECK(style.particles().size() == 9);
    for (const auto& p : style.particles()) {
        // Spawned 10 px outside an edge at 200-400 px/s, so at most 40 px inside it so far.
        CHECK(p.x >= -20.0);
        CHECK(p.x <= 320.0);
        CHECK(p.y >= -20.0);
        CHECK(p.y <= 100.0);
    }
    for (int i = 0; i < 60; ++i) {
        style.advance("transcribing", 1.0 / 30.0);
    }
    CHECK(!style.particles().empty());
    double mean_distance = 0.0;
    for (const auto& p : style.particles()) {
        mean_distance += std::hypot(p.x - 150, p.y - 40);
    }
    mean_distance /= static_cast<double>(style.particles().size());
    // The pull (about 500 px/s^2 and more) keeps them near the centre on average.
    CHECK(mean_distance < 80.0);
}

TEST_CASE("a run with the same seed repeats exactly", "[particles]") {
    auto run = [] {
        ParticleStyle style(300, 80, fv::overlay_particle_config(), std::mt19937(77));
        style.update_audio_levels({0.2});
        for (int i = 0; i < 45; ++i) {
            style.advance(i < 30 ? "recording" : "transcribing", 1.0 / 30.0);
        }
        return style.particles();
    };
    CHECK(run() == run());
}

TEST_CASE("an unknown state advances the clock and emits nothing", "[particles]") {
    ParticleStyle style(300, 80, ParticleConfig{}, std::mt19937(1));
    style.advance("idle", 0.1);
    CHECK(style.particles().empty());
    CHECK_THAT(style.animation_time(), WithinAbs(0.1, 1e-9));
}

TEST_CASE("the particle hue follows the animation clock", "[particles]") {
    ParticleStyle style(300, 80, ParticleConfig{}, std::mt19937(1));
    CHECK(style.hue() == 0.0);
    style.advance("idle", 0.1);
    CHECK_THAT(style.hue(), WithinAbs(style.animation_time() * 50.0, 1e-9));
}

TEST_CASE("the overlay's particle config is OpenWhisper's", "[particles]") {
    const ParticleConfig config = fv::overlay_particle_config();
    CHECK(config.max_particles == 150);
    CHECK(config.emission_rate == 30.0);
    CHECK(config.gravity == 20.0);
    CHECK(config.damping == 0.98);
    CHECK(config.wind_strength == 5.0);
    CHECK(config.audio_response == 1.5);
    CHECK(config.glow_effect);
    CHECK(config.turbulence_strength == 10.0);
    CHECK(config.color_shift_speed == 50.0);
}

TEST_CASE("a pinned clock sets the hue and the next tick runs on from it", "[particles]") {
    ParticleStyle style(300, 80, fv::overlay_particle_config(), std::mt19937(1));
    style.set_animation_time(3.0);
    CHECK(style.animation_time() == 3.0);
    CHECK_THAT(style.hue(), WithinAbs(150.0, 1e-12));
    style.advance("idle", 0.05);
    CHECK_THAT(style.animation_time(), WithinAbs(3.05, 1e-9));
}

TEST_CASE("a particle's colour stays defined outside a live particle's life", "[particles]") {
    fv::Particle p = particle_of(0, 0, 0, 0, 1e300, 1e300);
    CHECK(p.color(true).a == 255);
    CHECK(p.color(true) == fv::to_rgba({fv::hue_degrees(1e300), 200, 255}));
    p.life = -1e300;
    CHECK(p.color(true) == fv::to_rgba({fv::hue_degrees(1e300), 200, 0}));
    CHECK(p.color(false).a == 0);
}
