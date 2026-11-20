#pragma once

#include <array>
#include <optional>
#include <random>
#include <vector>

#include "uimath/ocean_school.h"

namespace fv {

// The Fizz look's simulation (ocean_looks.Fizz): nine glasses that fill as bubbles race up and
// reach the surface; the voice sets how many bubbles launch and how fast, and the level drains
// back in pauses. Its finish rolls the levels from glass to glass like a wave while the bubbles
// keep fizzing.
//
// Python draws the bubbles from random.Random(11): the engine passed in replays that stream
// (python_random_engine), and reset() rewinds it as a new recording does. Affinity: GUI thread;
// painting only reads it.
class FizzSim {
public:
    static constexpr int kLanes = 9;
    static constexpr double kBase = 50.0;
    static constexpr double kTop = 8.0;
    // The tallest a glass fills, in pixels.
    static constexpr double kMaxHeight = 38.0;
    static constexpr double kKick = 15.0;
    static constexpr double kGravity = 80.0;
    // Extra drain per pixel of fizz: quiet speech settles about a quarter full, ordinary speech
    // about three quarters, a raised voice at the brim.
    static constexpr double kLeak = 6.0;
    // How long a slosh rings on before the level settles.
    static constexpr double kSloshS = 0.35;
    // How long a splash ring lives.
    static constexpr double kSplashS = 0.3;

    struct Bubble {
        int lane = 0;
        double x = 0.0;
        double y = 0.0;
        double vy = 0.0;
        double r = 0.0;
    };
    // A bubble that reached the surface: when (the sim's clock), where.
    struct Splash {
        double start = 0.0;
        double x = 0.0;
        double y = 0.0;
    };
    using Levels = std::array<double, kLanes>;

    explicit FizzSim(double width = 300.0, std::mt19937 rng = python_random_engine(11));

    // A new recording: empty glasses, no bubbles.
    void reset();

    void set_width(double width) { width_ = width; }
    double width() const { return width_; }

    // One tick of dt seconds while recording; shown is VoiceRibbon::shown().
    void advance(double dt, double shown);

    // The recording ended: remembers the levels the finish eases away from.
    void begin_work();

    // One Processing/Transcribing tick: bubbles keep fizzing but no longer lift the levels.
    void advance_work(double dt, double rate);

    // The levels the finish draws at morph (0-1) and clock seconds: they ease from where they
    // were into a wave rolling across. As in Python, drawing them is what sets the glasses'
    // levels, and the bubbles' surface follows.
    const Levels& work_levels(double morph, double clock);

    // The glasses' fill, in pixels, and the velocity of each level.
    const Levels& levels() const { return h_; }
    const Levels& velocity() const { return v_; }
    const std::vector<Bubble>& bubbles() const { return bubbles_; }
    const std::vector<Splash>& splashes() const { return splashes_; }
    double time() const { return t_; }
    // The fraction of a bubble owed to the next tick's launches.
    double carry() const { return carry_; }

    // The width of one lane, and the x of its centre.
    double pitch() const;
    double lane_x(int lane) const;

private:
    void spawn(double rate, double speed, double dt);
    void rise(double dt, bool lift);

    double width_;
    std::mt19937 initial_rng_;
    std::mt19937 rng_;
    Levels h_{};
    Levels v_{};
    std::vector<Bubble> bubbles_;
    std::vector<Splash> splashes_;
    double t_ = 0.0;
    double carry_ = 0.0;
    std::optional<Levels> work_from_;
};

} // namespace fv
