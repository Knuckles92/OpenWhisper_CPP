#include "uimath/look_sims.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

#include "uimath/moments.h"
#include "uimath/overlay_look.h"

namespace fv {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kTau = 2.0 * std::numbers::pi;

// Python's float %, for a positive modulus.
double py_mod(double value, double modulus) {
    double result = std::fmod(value, modulus);
    if (result != 0.0 && result < 0.0) {
        result += modulus;
    }
    return result;
}

// Python's float // for a positive divisor.
double py_floordiv(double a, double b) {
    const double mod = std::fmod(a, b);
    double div = (a - mod) / b;
    if (mod != 0.0 && mod < 0.0) {
        div -= 1.0;
    }
    double floored = std::floor(div);
    if (div - floored > 0.5) {
        floored += 1.0;
    }
    return floored;
}

} // namespace

void VoiceSnapshot::take(std::vector<double> history) {
    values_ = std::move(history);
}

double VoiceSnapshot::at(double f) const {
    if (values_.empty()) {
        // Python indexes an empty snapshot and raises; a silent voice is the safe frame.
        return 0.0;
    }
    const auto last = static_cast<double>(values_.size() - 1);
    const double pos = detail::py_max(0.0, detail::py_min(1.0, f)) * last;
    const auto i = static_cast<std::size_t>(pos);
    const std::size_t j = std::min(i + 1, values_.size() - 1);
    return values_[i] + (values_[j] - values_[i]) * (pos - static_cast<double>(i));
}

RibbonFinish ribbon_finish(const VoiceSnapshot& snapshot, double width, double baseline, double hue,
                           double morph, double clock, bool dark) {
    const double spin = clock * 1.6 * kPi;
    const double cx = width / 2;
    const double cy = kCenterY - 2;
    constexpr int n = 80;
    std::vector<PointD> points;
    points.reserve(n + 1);
    for (int i = 0; i <= n; ++i) {
        const double f = static_cast<double>(i) / n;
        const double h = snapshot.at(f);
        const PointD line{kRibbonEdge + (width - 2 * kRibbonEdge) * f, baseline - kRibbonRise * h};
        const double angle = f * kTau + spin;
        const double r = 13.0 + 6.0 * h * (1.0 - 0.5 * morph);
        const PointD ring{cx + r * std::cos(angle) * 1.25, cy + r * std::sin(angle)};
        points.push_back(lerp_point(line, ring, morph));
    }
    RibbonFinish finish;
    for (int i = 0; i < n; ++i) {
        const double tail = static_cast<double>(i) / n;
        finish.segments.push_back(LineSegment{
            points[static_cast<std::size_t>(i)], points[static_cast<std::size_t>(i) + 1],
            hue_color(hue + tail * 90, 40.0 + 215.0 * tail, true, dark), 1.4 + 1.6 * tail});
    }
    finish.head = points.back();
    finish.head_radius = 2.6 * morph + 0.5;
    finish.head_color = hue_color(hue + 90, 255.0, false, dark);
    return finish;
}

std::vector<Bar> bars_frame(const std::vector<double>& history, double width, double hue,
                            double grow, bool dark) {
    const double span = width - 2 * kRibbonEdge;
    const int slots = std::max(1, static_cast<int>(py_floordiv(span, kBarPitch)));
    const auto length = static_cast<long long>(history.size());
    std::vector<Bar> bars;
    for (int i = 0; i < slots; ++i) {
        // Newest at the right; each bar is one history sample.
        const long long index =
            length - 1 - (static_cast<long long>(slots - 1 - i) * length) / slots;
        // An empty history (Python raises) draws silence.
        const double level =
            history.empty() ? 0.0 : history[static_cast<std::size_t>(std::max(0LL, index))];
        const double x = kRibbonEdge + i * kBarPitch;
        const double fade = detail::py_min(1.0, (x - kRibbonEdge) / (span * 0.25));
        const double half = (1.5 + kBarHalf * level) * grow;
        bars.push_back(Bar{RectD{x, kCenterY - half, kBarWidth, half * 2},
                           hue_color(hue + static_cast<double>(i) / slots * 60,
                                     (120.0 + 135.0 * level) * fade, true, dark)});
    }
    return bars;
}

std::vector<LineSegment> bars_finish(const VoiceSnapshot& snapshot, double width, double hue,
                                     double morph, double clock, bool dark) {
    const double cx = width / 2;
    const double cy = kCenterY - 1;
    const double head = py_mod(clock, 1.0);
    std::vector<LineSegment> spokes;
    for (int k = 0; k < kBarSpokes; ++k) {
        const double f = static_cast<double>(k) / kBarSpokes;
        const double h = snapshot.at(f);
        const double x0 = kRibbonEdge + (width - 2 * kRibbonEdge) * f;
        const double angle = f * kTau - kPi / 2;
        const double glow = detail::py_max(0.0, 1.0 - py_mod(f - head, 1.0) * 3.2);
        const double inner = 9.0;
        const double length = 4.0 + 9.0 * (0.35 * h + 0.65 * glow);
        // Each bar's ends travel to a spoke's inner and outer ends.
        const double cos_a = std::cos(angle);
        const double sin_a = std::sin(angle);
        const PointD bar_top{x0, kCenterY - (1.5 + kBarHalf * h)};
        const PointD bar_bottom{x0, kCenterY + (1.5 + kBarHalf * h)};
        const PointD spoke_in{cx + inner * cos_a, cy + inner * sin_a};
        const PointD spoke_out{cx + (inner + length) * cos_a, cy + (inner + length) * sin_a};
        const double alpha = 80.0 + 175.0 * detail::py_max(glow * morph, (1.0 - morph) * (0.4 + h));
        spokes.push_back(LineSegment{lerp_point(bar_top, spoke_in, morph),
                                     lerp_point(bar_bottom, spoke_out, morph),
                                     hue_color(hue + f * 60, alpha, true, dark), kBarWidth});
    }
    return spokes;
}

double wave_emitter(const VoiceRibbon& ribbon, double x, double overlay_width) {
    return kCenterY - kWaveAmplitude * ribbon.height_at(x, overlay_width);
}

WaveShape mirror_wave_frame(const VoiceRibbon& ribbon, double width, double hue, double grow,
                            bool dark) {
    constexpr int segments = 90;
    std::vector<double> xs;
    std::vector<double> amplitude;
    for (int i = 0; i <= segments; ++i) {
        const double x = kRibbonEdge + (width - 2 * kRibbonEdge) * i / segments;
        xs.push_back(x);
        amplitude.push_back((1.0 + kWaveAmplitude * ribbon.height_at(x, width)) * grow);
    }
    WaveShape shape;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        shape.outline.push_back(PointD{xs[i], kCenterY - amplitude[i]});
    }
    for (std::size_t i = xs.size(); i-- > 0;) {
        shape.outline.push_back(PointD{xs[i], kCenterY + amplitude[i]});
    }
    shape.gradient_left = xs.front();
    shape.gradient_right = xs.back();
    struct Stop {
        double position;
        double fill_alpha;
        double edge_alpha;
    };
    for (const Stop stop :
         {Stop{0.0, 0, 0}, Stop{0.15, 50, 140}, Stop{0.9, 90, 240}, Stop{1.0, 40, 200}}) {
        shape.fill.push_back({stop.position, hue_color(hue + stop.position * 60,
                                                       stop.fill_alpha * grow, true, dark)});
        shape.edge.push_back({stop.position, hue_color(hue + stop.position * 60,
                                                       stop.edge_alpha * grow, true, dark)});
    }
    return shape;
}

WaveShape mirror_wave_finish(const VoiceSnapshot& snapshot, double width, double hue, double morph,
                             double clock, bool dark) {
    const double pulse = 0.5 + 0.5 * std::sin(clock * 4.0);
    const double half_span = (width - 2 * kRibbonEdge) / 2;
    const double half_w = half_span + (26.0 + 8.0 * pulse - half_span) * morph;
    constexpr int segments = 60;
    std::vector<PointD> top;
    std::vector<PointD> bottom;
    for (int i = 0; i <= segments; ++i) {
        const double f = static_cast<double>(i) / segments;
        const double x = width / 2 - half_w + 2 * half_w * f;
        const double frozen = 1.0 + kWaveAmplitude * snapshot.at(f);
        const double capsule = (6.0 + 2.0 * pulse) * std::pow(std::sin(kPi * f), 0.35);
        const double a = frozen + (capsule - frozen) * morph;
        top.push_back(PointD{x, kCenterY - a});
        bottom.push_back(PointD{x, kCenterY + a});
    }
    WaveShape shape;
    shape.outline = top;
    shape.outline.insert(shape.outline.end(), bottom.rbegin(), bottom.rend());
    shape.gradient_left = width / 2 - half_w;
    shape.gradient_right = width / 2 + half_w;
    shape.fill = {{0.0, hue_color(hue, 120.0, true, dark)},
                  {0.5, hue_color(hue + 50, 230.0, true, dark)},
                  {1.0, hue_color(hue + 100, 120.0, true, dark)}};
    shape.pill = RectD{width / 2 - half_w - 6, kCenterY - 14, 2 * half_w + 12, 28};
    shape.pill_radius = 14.0;
    shape.pill_color = hue_color(hue + 50, 50.0 * pulse * morph, true, dark);
    return shape;
}

void AuroraSim::reset() {
    phase_ = 0.0;
    amplitude_ = 0.0;
}

void AuroraSim::advance(double dt, double level) {
    amplitude_ += (level - amplitude_) * (1.0 - std::exp(-dt / 0.05));
    phase_ += dt * (2.0 + 7.0 * level);
}

void AuroraSim::advance_work(double dt, double rate) {
    phase_ += dt * 6.0 * rate;
}

namespace {

// (frequency, phase shift, pen width) of the three waves.
struct WaveSpec {
    double freq;
    double shift;
    double pen;
};
constexpr WaveSpec kAuroraWaves[3] = {{2.2, 0.0, 2.2}, {3.1, 1.9, 1.6}, {1.6, 3.7, 1.2}};

AuroraWave aurora_wave(double width, double hue, double hue_drift, int k, double freq, double phase,
                       double amplitude, double pen, bool dark) {
    const double span = width - 2 * kRibbonEdge;
    AuroraWave wave;
    for (int i = 0; i <= 100; ++i) {
        const double envelope = std::pow(std::sin(kPi * i / 100), 1.4);
        const double y = kCenterY + amplitude * envelope * std::sin(freq * kTau * i / 100 + phase);
        wave.points.push_back(PointD{kRibbonEdge + span * i / 100, y});
    }
    const double color = hue + k * 45 + hue_drift;
    wave.glow_color = hue_color(color, 60.0, true, dark);
    wave.glow_width = pen + 4;
    wave.core_color = hue_color(color, 230.0, true, dark);
    wave.core_width = pen;
    return wave;
}

} // namespace

std::vector<AuroraWave> AuroraSim::frame(double width, double hue, double grow, bool dark) const {
    std::vector<AuroraWave> waves;
    for (int k = 0; k < 3; ++k) {
        const WaveSpec& spec = kAuroraWaves[k];
        const double phase = phase_ * (1 + 0.3 * k) + spec.shift;
        const double amplitude = (1.5 + 19.0 * amplitude_ * (1.0 - 0.22 * k)) * grow;
        waves.push_back(
            aurora_wave(width, hue, 0.0, k, spec.freq, phase, amplitude, spec.pen, dark));
    }
    return waves;
}

std::vector<AuroraWave> AuroraSim::finish(double width, double hue, double morph, double clock,
                                          bool dark) const {
    std::vector<AuroraWave> waves;
    for (int k = 0; k < 3; ++k) {
        const WaveSpec& spec = kAuroraWaves[k];
        const double braid_freq = spec.freq + (2.0 - spec.freq) * morph;
        const double braid_shift = spec.shift + (k * kTau / 3 - spec.shift) * morph;
        const double amplitude =
            (1.5 + 19.0 * amplitude_ * (1.0 - 0.22 * k)) * (1.0 - morph) + 9.0 * morph;
        waves.push_back(aurora_wave(width, hue, clock * 40, k, braid_freq, phase_ + braid_shift,
                                    amplitude, spec.pen, dark));
    }
    return waves;
}

void PulseSim::reset() {
    ripples_.clear();
    last_ = 0.0;
    last_ripple_ = -1.0;
    time_ = 0.0;
}

void PulseSim::advance(double dt, double level) {
    time_ += dt;
    // A word starting: the level jumps up past a threshold.
    if (level > 0.5 && level - last_ > 0.12 && time_ - last_ripple_ > 0.16) {
        ripples_.push_back(time_);
        last_ripple_ = time_;
    }
    last_ = level;
    std::erase_if(ripples_, [this](double r) { return !(time_ - r < kPulseRippleS); });
}

void PulseSim::advance_work(double dt, double /*rate*/) {
    time_ += dt;
    std::erase_if(ripples_, [this](double r) { return !(time_ - r < kPulseRippleS); });
}

std::vector<PulseCircle> PulseSim::ripple_circles(double width, double ring, double hue,
                                                  bool dark) const {
    std::vector<PulseCircle> circles;
    for (const double start : ripples_) {
        const double age = (time_ - start) / kPulseRippleS;
        PulseCircle c;
        c.center = PointD{width / 2, kCenterY - 1};
        c.radius = ring + 22.0 * ease_out_cubic(age);
        c.pen = hue_color(hue, 200.0 * std::pow(1.0 - age, 2.0), true, dark);
        c.pen_width = 1.6 * (1.0 - age) + 0.5;
        circles.push_back(c);
    }
    return circles;
}

PulseFrame PulseSim::frame(double width, double level, double hue, double grow, bool dark) const {
    const double pop = grow < 1.0 ? ease_out_back(grow) : 1.0;
    const double ring = (11.0 + 8.0 * level) * pop;
    const double core = (6.0 + 3.0 * level) * pop;
    const PointD center{width / 2, kCenterY - 1};
    PulseFrame frame;
    frame.ripples = ripple_circles(width, ring, hue, dark);
    frame.ring = PulseCircle{center, ring, hue_color(hue + 30, 230.0 * grow, true, dark),
                             2.0,    true, hue_color(hue, 60.0 * grow, true, dark)};
    frame.core =
        PulseCircle{center, core, Rgba{}, 0.0, true, hue_color(hue + 60, 255.0, false, dark)};
    return frame;
}

PulseFinish PulseSim::finish(double width, double hue, double morph, double clock,
                             bool dark) const {
    const PointD center{width / 2, kCenterY - 1};
    const double level = snapshot_.at(1.0);
    const double ring = (11.0 + 8.0 * level) + (14.0 - (11.0 + 8.0 * level)) * morph;
    PulseFinish out;
    out.ripples = ripple_circles(width, ring, hue, dark);
    // The full ring narrows to a comet that spins: the lit share of the gradient shrinks from
    // all of it to a quarter.
    const double lit = 1.0 - 0.72 * morph;
    const Rgba tail = hue_color(hue, 230.0 * (1.0 - morph), true, dark);
    out.sweep = {{0.0, hue_color(hue + 60, 255.0, false, dark)},
                 {std::max(0.01, lit * 0.35), hue_color(hue + 30, 220.0, true, dark)},
                 {std::max(0.02, lit), tail},
                 {1.0, tail}};
    out.sweep_angle_degrees = -(clock * 430);
    out.sweep_pen_width = 2.0 + 0.6 * morph;
    out.ring =
        PulseCircle{center, ring, hue_color(hue, 50.0 * morph, true, dark), 2.2, false, Rgba{}};
    const double core =
        (6.0 + 3.0 * level) * (1.0 - morph) + (4.5 + 0.8 * std::sin(clock * 5)) * morph;
    out.core =
        PulseCircle{center, core, Rgba{}, 0.0, true, hue_color(hue + 60, 255.0, false, dark)};
    return out;
}

namespace {

double dot_x(double width, int k) {
    return width / 2 + (k - (kDotCount - 1) / 2.0) * kDotSpacing;
}

double dot_level(const std::vector<double>& values, int k) {
    if (values.empty()) {
        return 0.0; // Python raises; draw silence
    }
    const long long index = static_cast<long long>(values.size()) - 1 -
                            static_cast<long long>(kDotCount - 1 - k) * kDotLag;
    return values[static_cast<std::size_t>(std::max(0LL, index))];
}

} // namespace

std::vector<Dot> dots_frame(const std::vector<double>& history, double width, double hue,
                            double grow, bool dark) {
    std::vector<Dot> dots;
    for (int k = 0; k < kDotCount; ++k) {
        // Dots pop in one after another as the recording starts.
        const double appear = grow < 1.0 ? ease_out_back(smooth(grow * 1.6 - k * 0.15)) : 1.0;
        const double level = dot_level(history, k);
        const double r = (3.5 + 3.5 * level) * detail::py_max(0.0, appear);
        Dot dot;
        dot.center = PointD{dot_x(width, k), kCenterY - 9 * level};
        dot.halo_radius = r + 4;
        dot.halo_color = hue_color(hue + k * 18, 70.0 * level, true, dark);
        dot.radius = r;
        dot.color = hue_color(hue + k * 18, 255.0, true, dark);
        dots.push_back(dot);
    }
    return dots;
}

std::vector<Dot> dots_finish(const VoiceSnapshot& snapshot, double width, double hue, double morph,
                             double clock, bool dark) {
    std::vector<Dot> dots;
    for (int k = 0; k < kDotCount; ++k) {
        const double frozen = dot_level(snapshot.values(), k);
        const double bounce = detail::py_max(0.0, std::sin(clock * 6.0 - k * 0.7));
        const double y = (kCenterY - 9 * frozen) +
                         ((kCenterY + 2 - 9 * bounce) - (kCenterY - 9 * frozen)) * morph;
        const double r = 3.5 + 1.8 * bounce * morph + 3.5 * frozen * (1.0 - morph);
        Dot dot;
        dot.center = PointD{dot_x(width, k), y};
        dot.halo_radius = r + 3;
        dot.halo_color = hue_color(hue + k * 18, 70.0 * bounce * morph, true, dark);
        dot.radius = r;
        dot.color = hue_color(hue + k * 18, 255.0, true, dark);
        dots.push_back(dot);
    }
    return dots;
}

} // namespace fv
