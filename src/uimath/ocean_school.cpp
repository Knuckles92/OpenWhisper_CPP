#include "uimath/ocean_school.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

#include "uimath/overlay_look.h"
#include "uimath/particles.h"
#include "uimath/voice_ribbon.h"

namespace fv {
namespace {

constexpr double kTau = 2.0 * std::numbers::pi;
// _School's tuning, in the Python's names.
constexpr double kSwell = 1.8;
constexpr double kRange = 26.0;
constexpr double kCohesion = 1.4;
constexpr double kBaitBallCohesion = 0.6;
constexpr double kAlign = 1.6;
constexpr double kSeparate = 260.0;
constexpr double kSteer = 15.0;
constexpr double kAttackS = 0.04;
constexpr double kReleaseS = 0.35;
// Room noise reads about 0.15 on the meter: below it the school rests.
constexpr double kVoiceFloor = 0.15;
constexpr double kBaitBallDepth = 7.0;
// Python's float % for a positive modulus.
double py_mod(double value, double modulus) {
    double result = std::fmod(value, modulus);
    if (result != 0.0 && result < 0.0) {
        result += modulus;
    }
    return result;
}

// math.hypot(x, y) as CPython 3.12 computes it: the squares are summed with their rounding errors
// kept and the root gets a correction step, so the result is almost always the correctly rounded
// one. The C library's hypot differs from it in the last bit now and then, and a school's
// flocking carries that into its next frames.
double py_hypot(double x, double y) {
    x = std::fabs(x);
    y = std::fabs(y);
    if (std::isinf(x) || std::isinf(y)) {
        return std::numeric_limits<double>::infinity();
    }
    if (std::isnan(x) || std::isnan(y)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double max = std::max(x, y);
    int max_e = 0;
    std::frexp(max, &max_e);
    if (max == 0.0 || max_e < -1023) {
        // Zero, or subnormal: CPython rescales; the C library's answer is as good here.
        return max == 0.0 ? max : std::hypot(x, y);
    }
    const double scale = std::ldexp(1.0, -max_e);
    double csum = 1.0;
    double frac1 = 0.0;
    double frac2 = 0.0;
    const auto add_square = [&](double v) {
        v *= scale;
        const double hi = v * v;
        const double lo = std::fma(v, v, -hi);
        const double sum = csum + hi;
        frac2 += (csum - sum) + hi;
        csum = sum;
        frac1 += lo;
    };
    add_square(x);
    add_square(y);
    double h = std::sqrt(csum - 1.0 + (frac1 + frac2));
    {
        const double hi = -h * h;
        const double lo = std::fma(-h, h, -hi);
        const double sum = csum + hi;
        frac2 += (csum - sum) + hi;
        csum = sum;
        frac1 += lo;
    }
    const double correction = csum - 1.0 + (frac1 + frac2);
    h += correction / (2.0 * h);
    return h / scale;
}

// sum() of floats as Python 3.12 computes it (Neumaier compensation).
double python_sum(const std::vector<Fish>& fish) {
    double total = 0.0;
    double compensation = 0.0;
    for (const Fish& f : fish) {
        const double value = f.y;
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

// What std::mt19937::seed(Sseq&) draws its 624 state words from: it takes the generated words
// as the state itself, which is how Python's init_by_array state is loaded.
class StateWords {
public:
    using result_type = std::uint32_t;

    explicit StateWords(const std::array<std::uint32_t, 624>& words) : words_(words) {}

    template <class It> void generate(It first, It last) {
        std::size_t i = 0;
        for (; first != last; ++first, ++i) {
            *first = words_[i % words_.size()];
        }
    }
    std::size_t size() const { return words_.size(); }

private:
    const std::array<std::uint32_t, 624>& words_;
};

} // namespace

std::mt19937 python_random_engine(std::uint32_t seed) {
    constexpr std::size_t n = 624;
    std::array<std::uint32_t, n> mt{};
    mt[0] = 19650218U;
    for (std::size_t i = 1; i < n; ++i) {
        mt[i] = 1812433253U * (mt[i - 1] ^ (mt[i - 1] >> 30)) + static_cast<std::uint32_t>(i);
    }
    // init_by_array with the one-word key [seed].
    std::size_t i = 1;
    for (std::size_t k = n; k != 0; --k) {
        mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525U)) + seed;
        ++i;
        if (i >= n) {
            mt[0] = mt[n - 1];
            i = 1;
        }
    }
    for (std::size_t k = n - 1; k != 0; --k) {
        mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941U)) -
                static_cast<std::uint32_t>(i);
        ++i;
        if (i >= n) {
            mt[0] = mt[n - 1];
            i = 1;
        }
    }
    mt[0] = 0x80000000U;
    std::mt19937 engine;
    StateWords words(mt);
    engine.seed(words);
    return engine;
}

int random_below(std::mt19937& rng, int n) {
    if (n < 1) {
        // randrange raises ValueError("empty range for randrange()").
        throw std::invalid_argument("random_below: empty range");
    }
    int bits = 0;
    for (unsigned v = static_cast<unsigned>(n); v != 0; v >>= 1) {
        ++bits;
    }
    int r = static_cast<int>(rng() >> (32 - bits));
    while (r >= n) {
        r = static_cast<int>(rng() >> (32 - bits));
    }
    return r;
}

SchoolSim::SchoolSim(SchoolKind kind, double width, std::mt19937 rng)
    : kind_(kind), width_(width), initial_rng_(rng), rng_(rng) {
    reset();
}

int SchoolSim::fish_count() const {
    return kind_ == SchoolKind::RealFish ? 44 : 56;
}

void SchoolSim::reset() {
    rng_ = initial_rng_;
    fish_.clear();
    const double width = std::max(200.0, width_);
    for (int i = 0; i < fish_count(); ++i) {
        // The school rises out of the deep as the recording starts: spread across the water,
        // low down, swimming up into place. The draws keep Python's order.
        Fish f;
        f.x = random_uniform(rng_, kRibbonEdge, width - kRibbonEdge);
        f.y = kBase - random_uniform(rng_, 0.5, 3.0);
        f.vx = random_uniform(rng_, -25.0, 25.0);
        f.vy = -random_uniform(rng_, 20.0, 60.0);
        f.r = random_uniform(rng_, 1.4, 3.8);
        f.hue = random_uniform(rng_, 0.0, 70.0);
        f.phase = random_uniform(rng_, 0.0, kTau);
        fish_.push_back(std::move(f));
    }
    for (auto& row : u_) {
        row.fill(0.0);
    }
    for (auto& row : uv_) {
        row.fill(0.0);
    }
    t_ = 0.0;
    level_ = 0.0;
    working_ = false;
    work_t_ = 0.0;
    rate_ = 1.0;
}

double SchoolSim::morph() const {
    return smooth(work_t_ / kMorphS);
}

double SchoolSim::voice() const {
    return working_ ? level_ * (1.0 - morph()) : level_;
}

void SchoolSim::advance(double dt, double shown) {
    if (kind_ == SchoolKind::BaitBall) {
        depth_ = python_sum(fish_) / static_cast<double>(fish_.size());
    }
    if (dt <= 0.0) {
        return;
    }
    t_ += dt;
    if (!working_) {
        // Ease level toward the voice: fast on a rise, slow on a fall.
        const double heard = detail::py_max(0.0, shown - kVoiceFloor) / (1.0 - kVoiceFloor);
        const double tau = heard > level_ ? kAttackS : kReleaseS;
        level_ += (heard - level_) * (1.0 - std::exp(-dt / tau));
    }
    swim(dt);
}

void SchoolSim::begin_work() {
    working_ = true;
    work_t_ = 0.0;
}

void SchoolSim::advance_work(double dt, double rate) {
    working_ = true;
    work_t_ += dt;
    rate_ = rate;
    if (kind_ == SchoolKind::BaitBall) {
        depth_ = python_sum(fish_) / static_cast<double>(fish_.size());
    }
    if (dt <= 0.0) {
        return;
    }
    t_ += dt;
    swim(dt);
}

void SchoolSim::swim(double dt) {
    const double lift = 4.0 + 40.0 * level_;
    const double speed = 18.0 + 70.0 * level_;
    const double blend = working_ ? morph() : 0.0;
    // Each fish moves in turn, so later fish flock to where earlier ones went.
    for (std::size_t i = 0; i < fish_.size(); ++i) {
        Fish& f = fish_[i];
        const auto [ax, ay] = flock(i);
        const auto [sx, sy] = heading(f, lift, speed, blend);
        f.vx += (ax + sx) * dt;
        f.vy += (ay + sy) * dt;
        f.vx *= std::pow(0.94, dt * 30.0);
        f.vy *= std::pow(0.9, dt * 30.0);
        cap_speed(f, speed);
        move(f, dt);
    }
    lift_layers(dt);
}

std::pair<double, double> SchoolSim::flock(std::size_t index) const {
    const Fish& f = fish_[index];
    const double cohesion = kind_ == SchoolKind::BaitBall ? kBaitBallCohesion : kCohesion;
    double ax = 0.0;
    double ay = 0.0;
    double cx = 0.0;
    double cy = 0.0;
    double avx = 0.0;
    double avy = 0.0;
    int n = 0;
    for (std::size_t j = 0; j < fish_.size(); ++j) {
        if (j == index) {
            continue;
        }
        const Fish& o = fish_[j];
        const double dx = o.x - f.x;
        const double dy = o.y - f.y;
        const double d2 = dx * dx + dy * dy;
        if (d2 >= std::pow(kRange, 2.0)) {
            continue;
        }
        ++n;
        cx += o.x;
        cy += o.y;
        avx += o.vx;
        avy += o.vy;
        if (0.01 < d2 && d2 < 49.0) {
            const double d = std::sqrt(d2);
            ax -= dx / d * kSeparate * (1.0 - d / 7.0);
            ay -= dy / d * kSeparate * (1.0 - d / 7.0);
        }
    }
    if (n != 0) {
        ax += (cx / n - f.x) * cohesion + (avx / n - f.vx) * kAlign;
        ay += (cy / n - f.y) * cohesion + (avy / n - f.vy) * kAlign;
    }
    return {ax, ay};
}

std::pair<double, double> SchoolSim::ball(double center_x, double center_y, double radius,
                                          double spin, const Fish& f) const {
    const double dx = f.x - center_x;
    const double dy = (f.y - center_y) * kBaitBallSquash;
    const double d = py_hypot(dx, dy) + 0.01;
    const double pull = (radius - d) * 6.0;
    return {dx / d * pull - dy / d * spin, (dy / d * pull + dx / d * spin) / kBaitBallSquash};
}

std::pair<double, double> SchoolSim::steer(const Fish& f, double lift, double speed) const {
    if (kind_ == SchoolKind::BaitBall) {
        // Quiet, the ball lies low and small; speaking swells it, lifts it and spins it up. The
        // depth pull moves every fish alike, keeping the shape.
        const double depth = kBase - 4.0 - lift * 0.85;
        const auto [ax, ay] = ball(width_ / 2 + std::sin(t_ * 0.35) * 70.0, depth,
                                   5.0 + 26.0 * level_, 50.0 + 230.0 * level_, f);
        return {ax, ay + (depth - depth_) * kBaitBallDepth};
    }
    // An altitude riding a wave, and a current.
    const double wave = 0.55 + 0.45 * std::sin(f.x * 0.035 - t_ * 2.2 + f.phase * 0.3);
    const double target = kBase - 3.0 - lift * wave;
    return {std::sin(t_ * 0.7 + f.y * 0.05) * speed * 0.8, (target - f.y) * kSteer};
}

std::pair<double, double> SchoolSim::steer_work(const Fish& f) const {
    switch (kind_) {
    case SchoolKind::BaitBall:
        // Pulls into the centre, tightens, and spins faster as the work runs.
        return ball(width_ / 2, kBase - 15.0, 7.0, 150.0 + 110.0 * rate_, f);
    case SchoolKind::RealFish: {
        // A slow wheel round the centre, like a school circling.
        const double cx = width_ / 2;
        const double cy = kBase - 13.0;
        const double dx = (f.x - cx) / 1.0;
        const double dy = (f.y - cy) * 3.0;
        const double d = py_hypot(dx, dy) + 0.01;
        const double pull = (40.0 - d) * 3.0;
        const double spin = 70.0 + 50.0 * rate_;
        return {dx / d * pull - dy / d * spin, (dy / d * pull + dx / d * spin) / 3.0};
    }
    case SchoolKind::DeepGlow:
        break;
    }
    // A whirl: each fish keeps to its own ring round the centre and spirals.
    const double cx = width_ / 2;
    const double cy = kBase - 15.0;
    const double dx = f.x - cx;
    const double dy = (f.y - cy) * 2.0;
    const double d = py_hypot(dx, dy) + 0.01;
    const double ring = 6.0 + 16.0 * (f.phase / kTau);
    const double pull = (ring - d) * 7.0;
    const double spin = 120.0 + 100.0 * rate_;
    return {dx / d * pull - dy / d * spin, (dy / d * pull + dx / d * spin) / 2.0};
}

std::pair<double, double> SchoolSim::heading(const Fish& f, double lift, double speed,
                                             double morph) const {
    if (!working_) {
        return steer(f, lift, speed);
    }
    const auto [rx, ry] = steer(f, lift, speed);
    const auto [wx, wy] = steer_work(f);
    return {rx + (wx - rx) * morph, ry + (wy - ry) * morph};
}

void SchoolSim::cap_speed(Fish& f, double speed) const {
    const double cap = !working_ ? (speed * 1.6 + 50.0) : 140.0 * rate_;
    const double sp = py_hypot(f.vx, f.vy);
    if (sp > cap) {
        f.vx *= cap / sp;
        f.vy *= cap / sp;
    }
}

void SchoolSim::move(Fish& f, double dt) const {
    if (kind_ == SchoolKind::DeepGlow) {
        f.trail.push_back(PointD{f.x, f.y});
        if (f.trail.size() > static_cast<std::size_t>(kDeepGlowTrail)) {
            f.trail.erase(f.trail.begin(),
                          f.trail.end() - static_cast<std::ptrdiff_t>(kDeepGlowTrail));
        }
    }
    f.x += f.vx * dt;
    f.y += f.vy * dt;
    if (f.x < kRibbonEdge) {
        f.x = kRibbonEdge;
        f.vx = std::fabs(f.vx);
    } else if (f.x > width_ - kRibbonEdge) {
        f.x = width_ - kRibbonEdge;
        f.vx = -std::fabs(f.vx);
    }
    f.y = detail::py_min(kBase - f.r, detail::py_max(kTop, f.y));
}

double SchoolSim::x_of(int p) const {
    return kRibbonEdge + (width_ - 2 * kRibbonEdge) * p / (kPoints - 1);
}

double SchoolSim::swell(int layer, double x) const {
    const double t = t_;
    return (kSwell * (1.0 + 1.2 * level_)) * (std::sin(x * 0.045 - t * 1.9 + layer * 0.7) * 0.7 +
                                              std::sin(x * 0.11 + t * 1.3 + layer * 1.9) * 0.3);
}

double SchoolSim::extra_lift(double x) const {
    // The bait ball's work heaves a dome of swell over its centre.
    if (kind_ != SchoolKind::BaitBall || !working_) {
        return 0.0;
    }
    const double offset = (x - width_ / 2) / 46.0;
    const double dome = std::exp(-std::pow(offset, 2.0));
    return dome * (5.0 + 1.5 * std::sin(work_t_ * 5.0 * rate_)) * morph();
}

double SchoolSim::y_of(int layer, int p) const {
    const double x = x_of(p);
    return kBottom - layer * kGap - u_[layer][p] - swell(layer, x) - extra_lift(x);
}

std::vector<PointD> SchoolSim::layer_points(int layer) const {
    std::vector<PointD> points;
    points.reserve(kPoints);
    for (int p = 0; p < kPoints; ++p) {
        points.push_back(PointD{x_of(p), y_of(layer, p)});
    }
    return points;
}

void SchoolSim::lift_layers(double dt) {
    // The highest the school reaches under each swell point.
    const double span = (width_ - 2 * kRibbonEdge) / (kPoints - 1);
    std::array<double, kPoints> tops;
    tops.fill(kBase);
    for (const Fish& f : fish_) {
        const int p = static_cast<int>(std::nearbyint((f.x - kRibbonEdge) / span));
        for (int q = p - 1; q <= p + 1; ++q) {
            if (q >= 0 && q < kPoints) {
                tops[q] = detail::py_min(tops[q], f.y - f.r - 1.0);
            }
        }
    }
    for (int step = 0; step < 2; ++step) {
        roll_springs(u_, uv_, dt / 2);
        for (int i = 0; i < kPoints; ++i) {
            push_column(i, tops[i]);
            clamp_column(i);
        }
    }
}

void SchoolSim::roll_springs(Layers& u, Layers& uv, double h) {
    constexpr int n = kPoints;
    for (int layer = 0; layer < kLayers; ++layer) {
        auto& ul = u[layer];
        auto& vl = uv[layer];
        std::array<double, n> acc;
        for (int i = 0; i < n; ++i) {
            const double left = i != 0 ? ul[i - 1] : ul[i];
            const double right = i < n - 1 ? ul[i + 1] : ul[i];
            acc[i] = -26.0 * ul[i] + 700.0 / 9.0 * (left + right - 2 * ul[i]) - 3.2 * vl[i];
        }
        for (int i = 0; i < n; ++i) {
            vl[i] += acc[i] * h;
            ul[i] += vl[i] * h;
        }
    }
}

void SchoolSim::push_column(int i, double top) {
    // The school under the bottom layer, then each layer under the next.
    const double y = y_of(0, i);
    if (top < y) {
        // Move out of the fish's way, with a little momentum, so a school that keeps nudging
        // does not pump the layer up.
        const double push = y - top;
        u_[0][i] += push * 0.4;
        uv_[0][i] = detail::py_max(uv_[0][i], push * 5.0);
    }
    for (int layer = 1; layer < kLayers; ++layer) {
        const double below = y_of(layer - 1, i);
        const double above = y_of(layer, i);
        if (above > below - kGapMin) {
            u_[layer][i] += above - (below - kGapMin);
            uv_[layer][i] = detail::py_max(uv_[layer][i], uv_[layer - 1][i] * 0.85);
        }
    }
}

void SchoolSim::clamp_column(int i) {
    // The stack never leaves the top: clamp in screen space, from the bottom layer up, so
    // layers keep their order there too.
    for (int layer = 0; layer < kLayers; ++layer) {
        const double ceiling = kTop + (kLayers - 1 - layer) * kGapMin;
        const double over = ceiling - y_of(layer, i);
        if (over > 0.0) {
            u_[layer][i] -= over;
            uv_[layer][i] = detail::py_min(uv_[layer][i], 0.0);
        }
        u_[layer][i] = detail::py_max(-4.0, u_[layer][i]);
    }
}

Rgba SchoolSim::ocean_color_at(double t, int layer, double alpha, double light, bool dark) {
    const double hue = 205.0 - layer * 9 + 10.0 * std::sin(t * 0.4);
    if (dark) {
        const double value = detail::py_min(255.0, 200.0 + layer * 11 + 30.0 * light);
        return to_rgba(
            Hsv{hue_degrees(hue), static_cast<int>(200.0 - layer * 18), static_cast<int>(value)},
            clamp_alpha(alpha));
    }
    return to_rgba(Hsv{hue_degrees(hue), static_cast<int>(235.0 - layer * 12),
                       static_cast<int>(170.0 + layer * 8)},
                   clamp_alpha(alpha));
}

Rgba SchoolSim::fish_color_at(SchoolKind kind, double t, const Fish& fish, double alpha,
                              double look_hue, bool dark) {
    if (kind == SchoolKind::DeepGlow) {
        const double hue = 160.0 + fish.hue * 0.6 + 15.0 * std::sin(t + fish.phase);
        return to_rgba(dark ? Hsv{hue_degrees(hue), 150, 255} : Hsv{hue_degrees(hue), 235, 190},
                       clamp_alpha(alpha));
    }
    const double hue = py_mod(look_hue + fish.hue, 360.0);
    // Fish the colour of the water would vanish into it, so near its hue they flash silver
    // (dark theme) or deepen to navy (light theme).
    const double near =
        detail::py_max(0.0, 1.0 - std::fabs(py_mod(hue - kWaterHue + 180.0, 360.0) - 180.0) / 90.0);
    const int degrees = static_cast<int>(hue);
    if (dark) {
        return to_rgba(Hsv{degrees, static_cast<int>(200.0 - 170.0 * near),
                           static_cast<int>(240.0 + 15.0 * near)},
                       clamp_alpha(alpha));
    }
    return to_rgba(Hsv{degrees, 235, static_cast<int>(205.0 - 85.0 * near)}, clamp_alpha(alpha));
}

} // namespace fv
