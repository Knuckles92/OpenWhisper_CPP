#pragma once

#include <array>
#include <cstdint>
#include <random>
#include <vector>

#include "uimath/color.h"

namespace fv {

// random.Random(seed) for an int seed below 2**32: CPython seeds its Mersenne Twister with
// init_by_array([seed]), which std::mt19937(seed) does not. The engine returned draws the same
// 32-bit words as Python's, so random_unit, random_uniform and random_below give Python's
// values, draw for draw.
std::mt19937 python_random_engine(std::uint32_t seed);

// random.randrange(n) for n >= 1: getrandbits(bit_length(n)), drawn again while >= n. Throws
// std::invalid_argument for n < 1, where Python raises ValueError.
int random_below(std::mt19937& rng, int n);

enum class SchoolKind { BaitBall, RealFish, DeepGlow };

// One fish of the school. trail holds the last kTrail positions of DeepGlow's fish, oldest
// first; it stays empty for the other kinds.
struct Fish {
    double x = 0.0;
    double y = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    double r = 0.0;
    double hue = 0.0;
    double phase = 0.0;
    std::vector<PointD> trail;
};

// The ocean looks' simulation (ocean_looks._School and its three kinds): a flocking school of
// balls under five stacked swell layers, each a row of 48 springs. The voice sets how high the
// school swims and how fast; the school pushes the bottom layer up where it crowds, and each
// layer pushes the one above. Positions are in the overlay's pixels, y down, in a band whose
// floor (kBase) is 50 and ceiling (kTop) 6.
//
// Python draws the fish from random.Random(5): the engine passed in replays that stream (see
// python_random_engine), and reset() rewinds it to the state it was constructed with, as a new
// recording does. Affinity: GUI thread (the overlay's frame timer); painting only reads it.
class SchoolSim {
public:
    static constexpr int kLayers = 5;
    static constexpr int kPoints = 48;
    static constexpr double kBase = 50.0;
    static constexpr double kTop = 6.0;
    // The lowest layer's rest height, and the rest spacing between layers.
    static constexpr double kBottom = 40.0;
    static constexpr double kGap = 5.5;
    // Closer than this and a layer shoves the one above.
    static constexpr double kGapMin = 2.6;
    static constexpr double kBaitBallSquash = 2.6;
    static constexpr int kDeepGlowTrail = 7;
    static constexpr double kDeepGlowPingS = 0.9;
    static constexpr double kWaterHue = 190.0;

    using Layers = std::array<std::array<double, kPoints>, kLayers>;

    // width is the width the look draws at (the overlay's, 300 unless embedded).
    explicit SchoolSim(SchoolKind kind, double width = 300.0,
                       std::mt19937 rng = python_random_engine(5));

    // A new recording: the school rises out of the deep, spread across the water and swimming
    // up into place; the swell is flat.
    void reset();

    // Takes effect from the next reset or advance.
    void set_width(double width) { width_ = width; }
    double width() const { return width_; }

    // One tick of dt seconds while recording; shown is VoiceRibbon::shown(). A dt of 0 or less
    // does nothing.
    void advance(double dt, double shown);

    // The recording ended: the school keeps swimming, morphing into the look's own work.
    void begin_work();

    // One Processing/Transcribing tick; rate eases up for Transcribing.
    void advance_work(double dt, double rate);

    SchoolKind kind() const { return kind_; }
    int fish_count() const;
    const std::vector<Fish>& fish() const { return fish_; }
    // The layer's spring displacement at each point, and its velocity.
    const Layers& displacement() const { return u_; }
    const Layers& velocity() const { return uv_; }
    // The sim's own clock, advanced by every tick.
    double time() const { return t_; }
    // The voice as the school feels it: a rise lands at once, a fall lingers (0-1).
    double level() const { return level_; }
    bool working() const { return working_; }
    double work_time() const { return work_t_; }
    double work_rate() const { return rate_; }

    // The work's morph, smooth(work_time / kMorphS).
    double morph() const;
    // How strongly the voice shows, easing out as the work morphs in.
    double voice() const;

    // The x of swell point p, and the y of layer at point p (the layer's rest height less its
    // displacement, swell and, for the bait ball's work, the dome).
    double x_of(int p) const;
    double y_of(int layer, int p) const;
    // The layer's polyline, one point per spring; the painter joins them with quadratic
    // curves through the midpoints, as layer_path does.
    std::vector<PointD> layer_points(int layer) const;

    // A layer's colour, deep blue at the bottom to sea-glass at the top. light (0-1) brightens
    // it on the dark theme.
    Rgba ocean_color(int layer, double alpha, double light, bool dark) const {
        return ocean_color_at(t_, layer, alpha, light, dark);
    }
    // A fish's colour. look_hue is the particles' hue (live_recording.particle_hue); DeepGlow's
    // fish ignore it.
    Rgba fish_color(const Fish& fish, double alpha, double look_hue, bool dark) const {
        return fish_color_at(kind_, t_, fish, alpha, look_hue, dark);
    }
    // The same at an explicit clock t, which is all the colours read of the sim.
    static Rgba ocean_color_at(double t, int layer, double alpha, double light, bool dark);
    static Rgba fish_color_at(SchoolKind kind, double t, const Fish& fish, double alpha,
                              double look_hue, bool dark);

    // One step of every layer's row of springs by h seconds. Exposed because it is the
    // deterministic part of the sim: it needs no fish.
    static void roll_springs(Layers& u, Layers& uv, double h);

private:
    void swim(double dt);
    void follow_voice(double dt);
    std::pair<double, double> flock(std::size_t index) const;
    std::pair<double, double> steer(const Fish& f, double lift, double speed) const;
    std::pair<double, double> steer_work(const Fish& f) const;
    std::pair<double, double> heading(const Fish& f, double lift, double speed, double morph) const;
    std::pair<double, double> ball(double center_x, double center_y, double radius, double spin,
                                   const Fish& f) const;
    void cap_speed(Fish& f, double speed) const;
    void move(Fish& f, double dt) const;
    double swell(int layer, double x) const;
    double extra_lift(double x) const;
    void lift_layers(double dt);
    void push_column(int i, double top);
    void clamp_column(int i);

    SchoolKind kind_;
    double width_;
    std::mt19937 initial_rng_;
    std::mt19937 rng_;
    std::vector<Fish> fish_;
    Layers u_{};
    Layers uv_{};
    double t_ = 0.0;
    double level_ = 0.0;
    bool working_ = false;
    double work_t_ = 0.0;
    double rate_ = 1.0;
    // The fish's mean height at the start of the last tick, which the bait ball steers its
    // whole shape by.
    double depth_ = 0.0;
};

} // namespace fv
