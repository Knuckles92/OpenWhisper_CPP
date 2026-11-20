#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "uimath/color.h"

namespace fv {

// random.random(): [0, 1) from 53 bits of two draws, as CPython builds it, so the value does
// not depend on the standard library's distribution classes.
double random_unit(std::mt19937& rng);
// random.uniform(a, b).
double random_uniform(std::mt19937& rng, double a, double b);
// random.randint(a, b), both ends included.
int random_int(std::mt19937& rng, int a, int b);

// One dot of the overlay's particle looks and one-shot bursts. Affinity: any single thread.
struct Particle {
    double x = 0.0;
    double y = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    double life = 1.0;
    double size = 0.0;
    double color_hue = 0.0;

    friend bool operator==(const Particle&, const Particle&) = default;

    // Particle(x, y, vx, vy, hue): size is uniform in 1.5-4.0 and, without hue, so is the hue
    // in 0-360, each drawn from rng in that order.
    static Particle spawn(std::mt19937& rng, double x, double y, double vx = 0.0, double vy = 0.0,
                          std::optional<double> hue = std::nullopt);

    // One step of dt seconds: moves, applies gravity to vy, damps both velocities by
    // damping ** (dt * 30) so the decay is the same at any frame rate, and ages by dt * 0.5.
    // False once life is spent.
    bool update(double dt, double gravity = 0.0, double damping = 0.99);

    // The particle's colour on the theme, for a live particle (0 < life <= 1; the alpha and
    // value are clamped to their ranges where Qt would return an invalid colour). base_hue
    // replaces the particle's own hue. Dark: dying particles darken into the pill. Light: they
    // fade out instead.
    Rgba color(bool dark, std::optional<double> base_hue = std::nullopt) const;

    // A colour that fades by alpha on both themes, for the swarms around a glyph.
    Rgba fading_color(bool dark) const;
};

// The settings of ParticleStyle; the defaults are the ones its `config.get(...)` falls back to.
struct ParticleConfig {
    int max_particles = 500;
    double emission_rate = 100.0;
    double gravity = 20.0;
    double damping = 0.98;
    double wind_strength = 5.0;
    double audio_response = 1.5;
    bool glow_effect = true;
    double turbulence_strength = 10.0;
    double color_shift_speed = 50.0;
};

// config.WAVEFORM_STYLE_CONFIGS["particle"]: what the overlay builds its style with.
ParticleConfig overlay_particle_config();

// The processing vortex's arms; 120 particles a second spread over them.
inline constexpr int kVortexArms = 4;
// Physics runs in steps of this length however long a tick is, so emission and motion do not
// depend on the display's frame rate.
inline constexpr double kParticleStepS = 1.0 / 120.0;
// A tick longer than this (a sleeping or hidden window) is cut to it, so there is no burst.
inline constexpr double kParticleMaxTickS = 0.1;
// The glow ring of a live particle (life > 0.5) has this alpha.
inline constexpr int kParticleGlowAlpha = 100;

// Physics-based waveform particles driven by audio energy. Affinity: GUI thread (the overlay's
// frame timer); painting only reads it.
class ParticleStyle {
public:
    // width and height are the overlay's size in pixels; rng is the source of every random
    // choice, so a test with a seed replays a run.
    ParticleStyle(int width, int height, const ParticleConfig& config, std::mt19937 rng);

    void update_audio_levels(const std::vector<double>& levels);

    // Advances the physics for one timer tick of delta_time seconds in the named state:
    // "recording" and "streaming" emit from the audio and drift, "processing" is the vortex,
    // "transcribing" converges on the centre, any other state emits nothing. A change of state
    // drops the emission remainder. Repainting never advances anything.
    void advance(std::string_view state, double delta_time);

    const std::vector<Particle>& particles() const { return particles_; }
    double animation_time() const { return animation_time_; }
    const std::vector<double>& audio_levels() const { return audio_levels_; }
    int width() const { return width_; }
    int height() const { return height_; }

    // The live settings, read every tick: the overlay changes audio_response and emission_rate
    // when the look changes.
    ParticleConfig config;

    // Where a recording particle launches from at x; unset keeps the flat line 30 px above
    // the bottom.
    std::function<double(double)> emitter;

    // The hue the particles are emitted around right now.
    double hue() const;

    // Pins the simulation clock (Python assigns style.animation_time): reference screenshots
    // and tests freeze the frame so the hue and the turbulence phase are known.
    void set_animation_time(double seconds) { animation_time_ = seconds; }

private:
    int emission_count(double rate, double dt);
    void advance_recording(double dt);
    void advance_processing(double dt);
    void advance_transcribing(double dt);
    void emit_audio_particles(int count, double audio_energy);
    enum class Mode { Drift, Vortex, Converge };
    void update_particles(double dt, double audio_energy, Mode mode);
    bool step_vortex(Particle& particle, double dt, double center_x, double center_y);
    bool step_converge(Particle& particle, double dt, double center_x, double center_y);
    bool step_drift(Particle& particle, double dt, double audio_energy);

    int width_;
    int height_;
    std::mt19937 rng_;
    double animation_time_ = 0.0;
    std::vector<double> audio_levels_;
    std::vector<Particle> particles_;
    double simulation_remainder_ = 0.0;
    double emission_remainder_ = 0.0;
    std::string simulation_state_ = "idle";
    int vortex_arm_ = 0;
};

} // namespace fv
