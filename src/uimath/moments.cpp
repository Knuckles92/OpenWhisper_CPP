#include "uimath/moments.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>

namespace fv {
namespace {

constexpr double kTau = 2.0 * std::numbers::pi;

double segment_length(PointD a, PointD b) {
    return std::hypot(b.x - a.x, b.y - a.y);
}

const std::array<Glyph, 4>& glyph_table() {
    static const std::array<Glyph, 4> table = {{
        {"check", {{{-5.5, 0.3}, {-1.8, 4.0}, {5.8, -4.4}}}, 2.6},
        {"cross", {{{-4.2, -4.2}, {4.2, 4.2}}, {{4.2, -4.2}, {-4.2, 4.2}}}, 2.6},
        {"bang", {{{0.0, -5.2}, {0.0, 1.4}}, {{0.0, 4.9}, {0.0, 4.95}}}, 2.4},
        {"clip",
         {{{-2.2, -5.6}, {-5.0, -5.6}, {-5.0, 6.4}, {5.0, 6.4}, {5.0, -5.6}, {2.2, -5.6}},
          {{-2.6, -7.2}, {2.6, -7.2}, {2.6, -4.2}, {-2.6, -4.2}, {-2.6, -7.2}},
          {{-2.6, -0.6}, {2.6, -0.6}},
          {{-2.6, 2.8}, {1.4, 2.8}}},
         1.5},
    }};
    return table;
}

const std::array<NamedMoment, 4>& moment_table() {
    static const std::array<NamedMoment, 4> table = {{
        {"done",
         {"success",
          "purple",
          "check",
          "sparkles",
          0.22,
          {{{130, 150}, {272, 292}}},
          "purple-text",
          0.0,
          true}},
        {"enabled",
         {"success",
          "accent-cyan",
          "check",
          "converge",
          0.3,
          {{{128, 150}, {182, 198}}},
          "success-text",
          0.0,
          false}},
        {"disabled",
         {"danger",
          "warning",
          "cross",
          "",
          0.0,
          {{{0, 10}, {24, 38}}},
          "danger-text",
          260.0,
          false}},
        {"copied",
         {"accent-cyan",
          "accent",
          "clip",
          "converge",
          0.3,
          {{{188, 200}, {206, 218}}},
          "accent-cyan",
          0.0,
          false}},
    }};
    return table;
}

} // namespace

const Glyph& glyph(std::string_view name) {
    for (const Glyph& candidate : glyph_table()) {
        if (candidate.name == name) {
            return candidate;
        }
    }
    throw std::out_of_range("no glyph named " + std::string(name));
}

double GlyphPath::length() const {
    double total = 0.0;
    for (const auto& stroke : strokes) {
        for (std::size_t i = 1; i < stroke.size(); ++i) {
            total += segment_length(stroke[i - 1], stroke[i]);
        }
    }
    return total;
}

GlyphPath glyph_path(std::string_view name, PointD center, double amount) {
    const Glyph& definition = glyph(name);
    std::vector<std::vector<PointD>> strokes;
    double total = 0.0;
    for (const auto& stroke : definition.strokes) {
        std::vector<PointD> moved;
        for (const PointD& point : stroke) {
            moved.push_back(PointD{center.x + point.x, center.y + point.y});
        }
        for (std::size_t i = 1; i < moved.size(); ++i) {
            total += segment_length(moved[i - 1], moved[i]);
        }
        strokes.push_back(std::move(moved));
    }
    double remaining = amount * total;
    GlyphPath path;
    for (const auto& stroke : strokes) {
        if (remaining <= 0.0) {
            break;
        }
        std::vector<PointD> drawn{stroke.front()};
        for (std::size_t i = 1; i < stroke.size(); ++i) {
            const PointD start = stroke[i - 1];
            const PointD end = stroke[i];
            const double length = segment_length(start, end);
            const double step = detail::py_min(1.0, remaining / detail::py_max(length, 1e-6));
            drawn.push_back(
                PointD{start.x + (end.x - start.x) * step, start.y + (end.y - start.y) * step});
            remaining -= length;
            if (remaining <= 0.0) {
                break;
            }
        }
        path.strokes.push_back(std::move(drawn));
    }
    return path;
}

std::span<const NamedMoment> moments() {
    return moment_table();
}

const Moment* find_moment(std::string_view key) {
    for (const NamedMoment& named : moment_table()) {
        if (named.key == key) {
            return &named.moment;
        }
    }
    return nullptr;
}

const Moment& canceled_moment() {
    static const Moment canceled{"danger",      "warning", "cross",
                                 "collapse",    0.16,      {{{0, 12}, {18, 40}}},
                                 "danger-text", 0.0,       false};
    return canceled;
}

std::vector<Particle> burst(const Moment& moment, double cx, double cy, std::mt19937& rng,
                            int count) {
    std::vector<Particle> particles;
    for (int i = 0; i < count; ++i) {
        const double angle = static_cast<double>(i) / count * kTau + random_uniform(rng, -0.3, 0.3);
        const double speed = random_uniform(rng, 170, 280);
        const double x = cx + random_uniform(rng, -3, 3);
        const double y = cy + random_uniform(rng, -3, 3);
        const HueRange range = moment.hues[static_cast<std::size_t>(i % 2)];
        const double hue = random_uniform(rng, range.low, range.high);
        Particle particle = Particle::spawn(rng, x, y, std::cos(angle) * speed,
                                            std::sin(angle) * speed * 0.35, hue);
        particle.size = random_uniform(rng, 1.6, 3.0);
        particles.push_back(particle);
    }
    return particles;
}

void advance_confetti(std::vector<Particle>& particles, double dt, double fall) {
    std::vector<Particle> alive;
    alive.reserve(particles.size());
    for (Particle& particle : particles) {
        particle.vy += fall * dt;
        particle.life -= dt * 0.6;
        if (particle.update(dt, 0.0, 0.92)) {
            alive.push_back(particle);
        }
    }
    particles = std::move(alive);
}

std::vector<Particle> bubble_burst(const Moment& moment, double cx, double cy, std::mt19937& rng,
                                   int count) {
    std::vector<Particle> particles;
    for (int i = 0; i < count; ++i) {
        const double angle = random_uniform(rng, 0, kTau);
        const double speed = random_uniform(rng, 190, 360);
        const HueRange range = moment.hues[static_cast<std::size_t>(i % 2)];
        const double hue = random_uniform(rng, range.low, range.high);
        Particle particle = Particle::spawn(rng, cx, cy, std::cos(angle) * speed,
                                            std::sin(angle) * speed * 0.6, hue);
        particle.size = random_uniform(rng, 2.5, 5.0);
        particles.push_back(particle);
    }
    return particles;
}

void advance_bubbles(std::vector<Particle>& particles, double dt, std::mt19937& rng) {
    std::vector<Particle> alive;
    alive.reserve(particles.size());
    for (Particle& particle : particles) {
        particle.vx += random_uniform(rng, -25, 25) * dt;
        particle.vy += random_uniform(rng, -25, 25) * dt;
        particle.life -= dt * 0.35;
        if (particle.update(dt, 0.0, 0.93)) {
            alive.push_back(particle);
        }
    }
    particles = std::move(alive);
}

} // namespace fv
