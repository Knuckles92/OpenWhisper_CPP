#include "uimath/particles.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace fv {
namespace {

// Python's float % for a positive modulus: the result is never negative.
double py_fmod(double value, double modulus) {
    double result = std::fmod(value, modulus);
    if (result != 0.0 && result < 0.0) {
        result += modulus;
    }
    return result;
}

// sum() of floats as Python 3.12 computes it (Neumaier compensation), so the mean of a level
// list matches the reference to the last bit.
double python_sum(const std::vector<double>& values) {
    double total = 0.0;
    double compensation = 0.0;
    for (const double value : values) {
        const double t = total + value;
        if (std::fabs(total) >= std::fabs(value)) {
            compensation += (total - t) + value;
        } else {
            compensation += (value - t) + total;
        }
        total = t;
    }
    if (std::isfinite(compensation)) {
        total += compensation;
    }
    return total;
}

} // namespace

double random_unit(std::mt19937& rng) {
    const std::uint32_t high = rng() >> 5;
    const std::uint32_t low = rng() >> 6;
    return (static_cast<double>(high) * 67108864.0 + static_cast<double>(low)) *
           (1.0 / 9007199254740992.0);
}

double random_uniform(std::mt19937& rng, double a, double b) {
    return a + (b - a) * random_unit(rng);
}

int random_int(std::mt19937& rng, int a, int b) {
    return a + static_cast<int>(random_unit(rng) * static_cast<double>(b - a + 1));
}

Particle Particle::spawn(std::mt19937& rng, double x, double y, double vx, double vy,
                         std::optional<double> hue) {
    Particle particle;
    particle.x = x;
    particle.y = y;
    particle.vx = vx;
    particle.vy = vy;
    particle.size = random_uniform(rng, 1.5, 4.0);
    particle.color_hue = hue ? *hue : random_uniform(rng, 0.0, 360.0);
    return particle;
}

bool Particle::update(double dt, double gravity, double damping) {
    x += vx * dt;
    y += vy * dt;
    vy += gravity * dt;

    const double frame_damping = std::pow(damping, dt * 30.0);
    vx *= frame_damping;
    vy *= frame_damping;

    life -= dt * 0.5;
    return life > 0.0;
}

Rgba Particle::color(bool dark, std::optional<double> base_hue) const {
    const int hue = hue_degrees(base_hue ? *base_hue : color_hue);
    if (dark) {
        // Clamped before the conversion: Qt's range is 0-255 and a double past int's range
        // must not reach static_cast.
        const double value = std::clamp(life * 230.0 + 25.0, 0.0, 255.0);
        return to_rgba(Hsv{hue, 200, std::isnan(value) ? 0 : static_cast<int>(value)});
    }
    return to_rgba(Hsv{hue, 235, 205}, clamp_alpha(life * 255.0));
}

Rgba Particle::fading_color(bool dark) const {
    const int alpha = clamp_alpha(255.0 * life);
    const int hue = hue_degrees(color_hue);
    if (dark) {
        return to_rgba(Hsv{hue, 200, 230}, alpha);
    }
    return to_rgba(Hsv{hue, 235, 205}, alpha);
}

ParticleConfig overlay_particle_config() {
    ParticleConfig config;
    config.max_particles = 150;
    config.emission_rate = 30.0;
    config.gravity = 20.0;
    config.damping = 0.98;
    config.wind_strength = 5.0;
    config.audio_response = 1.5;
    config.glow_effect = true;
    config.turbulence_strength = 10.0;
    config.color_shift_speed = 50.0;
    return config;
}

ParticleStyle::ParticleStyle(int width, int height, const ParticleConfig& config_in,
                             std::mt19937 rng)
    : config(config_in), width_(width), height_(height), rng_(std::move(rng)) {}

void ParticleStyle::update_audio_levels(const std::vector<double>& levels) {
    audio_levels_ = levels;
}

double ParticleStyle::hue() const {
    return py_fmod(animation_time_ * config.color_shift_speed, 360.0);
}

void ParticleStyle::advance(std::string_view state, double delta_time) {
    delta_time = detail::py_max(0.0, detail::py_min(kParticleMaxTickS, delta_time));
    if (state != simulation_state_) {
        simulation_state_ = std::string(state);
        emission_remainder_ = 0.0;
    }
    simulation_remainder_ += delta_time;
    const double step = kParticleStepS;
    while (simulation_remainder_ + 1e-12 >= step) {
        simulation_remainder_ -= step;
        animation_time_ += step;
        if (state == "recording" || state == "streaming") {
            advance_recording(step);
        } else if (state == "processing") {
            advance_processing(step);
        } else if (state == "transcribing") {
            advance_transcribing(step);
        }
    }
}

int ParticleStyle::emission_count(double rate, double dt) {
    emission_remainder_ += rate * dt;
    const int count = static_cast<int>(emission_remainder_ + 1e-12);
    emission_remainder_ -= count;
    return count;
}

void ParticleStyle::advance_recording(double dt) {
    const double audio_energy =
        audio_levels_.empty()
            ? 0.0
            : python_sum(audio_levels_) / static_cast<double>(audio_levels_.size());

    const double emission_multiplier = 1.0 + audio_energy * config.audio_response;
    const int to_emit = emission_count(config.emission_rate * emission_multiplier, dt);

    emit_audio_particles(to_emit, audio_energy);
    update_particles(dt, audio_energy, Mode::Drift);
}

void ParticleStyle::advance_processing(double dt) {
    const int center_x = width_ / 2;
    const int center_y = height_ / 2 - 5;

    const int count = emission_count(120.0, dt);
    for (int i = 0; i < count; ++i) {
        // Each particle takes the next of the vortex's arms. At 120 Hz a step emits about one,
        // so spreading by its index within the step put every particle on the same arm: one
        // comet, not a vortex.
        const int arm = vortex_arm_;
        vortex_arm_ = (arm + 1) % kVortexArms;
        const double angle =
            static_cast<double>(arm) / kVortexArms * 2 * std::numbers::pi + animation_time_ * 2;
        const double radius = 30 + 10 * std::sin(animation_time_ * 3);

        const double x = center_x + radius * std::cos(angle);
        const double y = center_y + radius * std::sin(angle);

        const double vx = -std::sin(angle) * 50;
        const double vy = std::cos(angle) * 50;

        Particle particle = Particle::spawn(rng_, x, y, vx, vy);
        particle.color_hue = py_fmod(angle * 180 / std::numbers::pi + animation_time_ * 50, 360.0);
        particles_.push_back(particle);
    }

    update_particles(dt, 0.5, Mode::Vortex);
}

void ParticleStyle::advance_transcribing(double dt) {
    const int count = emission_count(90.0, dt);
    for (int i = 0; i < count; ++i) {
        const int edge = random_int(rng_, 0, 3);
        double x = 0.0;
        double y = 0.0;
        if (edge == 0) {
            x = random_uniform(rng_, 0, width_);
            y = -10;
        } else if (edge == 1) {
            x = width_ + 10;
            y = random_uniform(rng_, 0, height_);
        } else if (edge == 2) {
            x = random_uniform(rng_, 0, width_);
            y = height_ + 10;
        } else {
            x = -10;
            y = random_uniform(rng_, 0, height_);
        }

        const int center_x = width_ / 2;
        const int center_y = height_ / 2;
        const double angle = std::atan2(center_y - y, center_x - x);

        const double speed = random_uniform(rng_, 200, 400);
        const double vx = std::cos(angle) * speed;
        const double vy = std::sin(angle) * speed;

        Particle particle = Particle::spawn(rng_, x, y, vx, vy);
        particle.color_hue = random_uniform(rng_, 0, 360);
        particles_.push_back(particle);
    }

    update_particles(dt, 0.3, Mode::Converge);
}

void ParticleStyle::emit_audio_particles(int count, double audio_energy) {
    const int room = config.max_particles - static_cast<int>(particles_.size());
    const int emit = std::min(count, room);
    for (int i = 0; i < emit; ++i) {
        const double x = random_uniform(rng_, 20, width_ - 20);
        const double y = emitter ? emitter(x) : height_ - 30;

        const double vx = random_uniform(rng_, -30, 30) * (1 + audio_energy);
        const double vy = random_uniform(rng_, -80, -40) * (1 + audio_energy * 0.5);

        Particle particle = Particle::spawn(rng_, x, y, vx, vy);
        particle.color_hue = py_fmod(
            animation_time_ * config.color_shift_speed + random_uniform(rng_, 0, 60), 360.0);
        particles_.push_back(particle);
    }
}

void ParticleStyle::update_particles(double dt, double audio_energy, Mode mode) {
    const double center_x = width_ / 2;
    const double center_y = height_ / 2;

    std::vector<Particle> alive;
    alive.reserve(particles_.size());
    for (Particle& particle : particles_) {
        bool survives = false;
        switch (mode) {
        case Mode::Vortex:
            survives = step_vortex(particle, dt, center_x, center_y);
            break;
        case Mode::Converge:
            survives = step_converge(particle, dt, center_x, center_y);
            break;
        case Mode::Drift:
            survives = step_drift(particle, dt, audio_energy);
            break;
        }
        if (survives) {
            alive.push_back(particle);
        }
    }

    particles_ = std::move(alive);
    // Python keeps particles[-max:], which for a maximum of 0 is the whole list.
    if (config.max_particles > 0 && static_cast<int>(particles_.size()) > config.max_particles) {
        particles_.erase(particles_.begin(),
                         particles_.end() - static_cast<std::ptrdiff_t>(config.max_particles));
    }
}

bool ParticleStyle::step_vortex(Particle& particle, double dt, double center_x, double center_y) {
    // Swirl around the centre while drifting slowly inward.
    const double dx = particle.x - center_x;
    const double dy = particle.y - center_y;
    const double distance = std::sqrt(dx * dx + dy * dy);

    if (distance > 0) {
        const double radial_force = -50 / (distance + 1);
        particle.vx += (dx / distance) * radial_force * dt;
        particle.vy += (dy / distance) * radial_force * dt;

        const double tangent_force = 100;
        particle.vx += (-dy / distance) * tangent_force * dt;
        particle.vy += (dx / distance) * tangent_force * dt;
    }

    return particle.update(dt, 0, 0.95);
}

bool ParticleStyle::step_converge(Particle& particle, double dt, double center_x, double center_y) {
    // Pull into the centre with a swirl; a particle that arrives fades out.
    const double dx = center_x - particle.x;
    const double dy = center_y - particle.y;
    const double distance = std::sqrt(dx * dx + dy * dy);

    if (distance > 5) {
        const double nx = dx / distance;
        const double ny = dy / distance;

        const double attraction = 15000 / (distance + 10) + 500;

        const double swirl = 300;

        particle.vx += (nx * attraction - ny * swirl) * dt;
        particle.vy += (ny * attraction + nx * swirl) * dt;

        particle.vx *= std::pow(0.9, dt * 30.0);
        particle.vy *= std::pow(0.9, dt * 30.0);
    } else {
        particle.life -= dt * 5.0;
    }

    return particle.update(dt, 0, 1.0);
}

bool ParticleStyle::step_drift(Particle& particle, double dt, double audio_energy) {
    // Rise on turbulence and wind, bouncing off the sides and the floor.
    const double turbulence_multiplier = 1.0 + audio_energy * 2;
    const double turbulence_x = std::sin(animation_time_ * 3 + particle.x * 0.1) *
                                config.turbulence_strength * turbulence_multiplier;
    const double turbulence_y = std::cos(animation_time_ * 2.5 + particle.y * 0.1) *
                                config.turbulence_strength * turbulence_multiplier * 0.7;

    particle.vx += turbulence_x * dt;
    particle.vy += turbulence_y * dt;

    const double wind_x = std::sin(animation_time_) * config.wind_strength;
    particle.vx += wind_x * dt;

    if (particle.x <= 0 || particle.x >= width_) {
        particle.vx *= -0.8;
        particle.x = detail::py_max(0.0, detail::py_min(static_cast<double>(width_), particle.x));
    }

    if (particle.y >= height_ - 25) {
        particle.vy *= -0.6;
        particle.y = height_ - 25;
    }

    return particle.update(dt, config.gravity, config.damping);
}

} // namespace fv
