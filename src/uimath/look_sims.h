#pragma once

#include <utility>
#include <vector>

#include "uimath/color.h"
#include "uimath/voice_ribbon.h"

// The state and the geometry of the recording looks that are not the ocean ones
// (ui_qt/overlays/recording_looks.py): what each look would hand a painter, in the overlay's
// pixels with y down and centred on kCenterY, so the widget only has to paint it. Colours come
// from hue_color on the theme (dark picks the colour branch); the ribbon's own band is
// VoiceRibbon::shape(). hue is the particles' hue (particle_hue); grow (0-1) is the look's
// grow-in; morph (0-1) is how far the look has turned into its work and clock the work's own
// seconds.

namespace fv {

struct RectD {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    friend bool operator==(const RectD&, const RectD&) = default;
};

// A look's snapshot of the voice, taken when the recording ends (Visual.begin_work keeps the
// ribbon's history): the finish morphs from it.
class VoiceSnapshot {
public:
    // Two zeros until the first take.
    VoiceSnapshot() : values_(2, 0.0) {}

    void take(std::vector<double> history);
    const std::vector<double>& values() const { return values_; }

    // The frozen voice at 0-1 across, interpolated between samples; f is clamped. An empty
    // take reads 0 everywhere (Python would raise IndexError).
    double at(double f) const;

private:
    std::vector<double> values_;
};

// A straight stroke with round caps.
struct LineSegment {
    PointD from;
    PointD to;
    Rgba color;
    double width = 0.0;
};

// --- Ribbon: the band is VoiceRibbon::shape(); the finish rolls it into a spinning loop ---

struct RibbonFinish {
    // 80 segments, each a pen of its own colour (round caps), tail to head.
    std::vector<LineSegment> segments;
    // The head dot: a filled circle, no outline.
    PointD head;
    double head_radius = 0.0;
    Rgba head_color;
};
// baseline is overlay height less kBaselineFromBottom.
RibbonFinish ribbon_finish(const VoiceSnapshot& snapshot, double width, double baseline, double hue,
                           double morph, double clock, bool dark);

// --- Bars: a scrolling meter of rounded bars, mirrored about the middle ---

inline constexpr double kBarPitch = 5.0;
inline constexpr double kBarWidth = 3.0;
inline constexpr double kBarHalf = 18.0;
inline constexpr int kBarSpokes = 28;

struct Bar {
    // The rounded rectangle (x, kCenterY - half, kBarWidth, 2 * half), corner radius
    // kBarWidth / 2, filled with color.
    RectD rect;
    Rgba color;
};
std::vector<Bar> bars_frame(const std::vector<double>& history, double width, double hue,
                            double grow, bool dark);
// The finish: each bar's ends travel to a ring's spokes, a highlight running round. Lines with
// round caps of width kBarWidth.
std::vector<LineSegment> bars_finish(const VoiceSnapshot& snapshot, double width, double hue,
                                     double morph, double clock, bool dark);

// --- Mirror wave: the voice's outline as a glowing shape, mirrored ---

inline constexpr double kWaveAmplitude = 18.0;

struct WaveShape {
    // The closed outline: the top edge left to right, then the bottom edge right to left.
    std::vector<PointD> outline;
    // The fill's gradient runs horizontally from gradient_left to gradient_right.
    double gradient_left = 0.0;
    double gradient_right = 0.0;
    std::vector<GradientStop> fill;
    // The outline's pen (width edge_pen_width) uses this gradient over the same line; empty for
    // the finish, which only fills.
    std::vector<GradientStop> edge;
    double edge_pen_width = 1.4;
    // The finish's soft capsule behind the shape; width 0 means there is none (the frame).
    RectD pill;
    double pill_radius = 0.0;
    Rgba pill_color;
};
// Where the particles launch at x: the wave's top edge.
double wave_emitter(const VoiceRibbon& ribbon, double x, double overlay_width);
WaveShape mirror_wave_frame(const VoiceRibbon& ribbon, double width, double hue, double grow,
                            bool dark);
// The finish: the outline gathers into a short glowing capsule that pulses.
WaveShape mirror_wave_finish(const VoiceSnapshot& snapshot, double width, double hue, double morph,
                             double clock, bool dark);

// --- Aurora: three flowing waves whose height and pace follow the voice ---

struct AuroraWave {
    std::vector<PointD> points; // 101 points
    // A soft wide pen under a bright one, both with round caps.
    Rgba glow_color;
    double glow_width = 0.0;
    Rgba core_color;
    double core_width = 0.0;
};

class AuroraSim {
public:
    AuroraSim() { reset(); }

    void reset();
    // One recording tick; level is VoiceRibbon::shown().
    void advance(double dt, double level);
    // Nothing to keep: the finish morphs from the live waves.
    void begin_work() {}
    void advance_work(double dt, double rate);

    // The smoothed level (tau 0.05 s) and the waves' phase (pace 2 + 7 * level).
    double amplitude() const { return amplitude_; }
    double phase() const { return phase_; }

    std::vector<AuroraWave> frame(double width, double hue, double grow, bool dark) const;
    // The three waves twist into one braid that spins, their hue drifting.
    std::vector<AuroraWave> finish(double width, double hue, double morph, double clock,
                                   bool dark) const;

private:
    double phase_ = 0.0;
    double amplitude_ = 0.0;
};

// --- Pulse: a core and a ring that follow the voice, with round ripples ---

inline constexpr double kPulseRippleS = 0.8;

struct PulseCircle {
    PointD center;
    double radius = 0.0;
    // The outline's pen (none when pen_width is 0), and the fill (none unless filled).
    Rgba pen;
    double pen_width = 0.0;
    bool filled = false;
    Rgba fill;
};

struct PulseFrame {
    // The ripples, oldest first: rings with no fill.
    std::vector<PulseCircle> ripples;
    // The ring (outlined, tinted) and the solid core.
    PulseCircle ring;
    PulseCircle core;
};

struct PulseFinish {
    std::vector<PulseCircle> ripples;
    // The faint full ring, drawn first.
    PulseCircle ring;
    // The comet: the same ring stroked with a conical gradient centred on ring.center, started
    // at sweep_angle_degrees (Qt's counter-clockwise degrees), stops as (position, colour).
    std::vector<GradientStop> sweep;
    double sweep_angle_degrees = 0.0;
    double sweep_pen_width = 0.0;
    PulseCircle core;
};

class PulseSim {
public:
    PulseSim() { reset(); }

    void reset();
    // One recording tick; level is VoiceRibbon::shown(). A ripple starts when a word does: the
    // level is over 0.5, has jumped more than 0.12 since the last tick, and 0.16 s have passed
    // since the last ripple. Each lives kPulseRippleS.
    void advance(double dt, double level);
    // Keeps what the voice last looked like.
    void begin_work(std::vector<double> history) { snapshot_.take(std::move(history)); }
    void advance_work(double dt, double rate);

    // The start times (on the sim's clock) of the live ripples, oldest first.
    const std::vector<double>& ripples() const { return ripples_; }
    double time() const { return time_; }
    const VoiceSnapshot& snapshot() const { return snapshot_; }

    PulseFrame frame(double width, double level, double hue, double grow, bool dark) const;
    // The full ring narrows to a comet that spins round the core.
    PulseFinish finish(double width, double hue, double morph, double clock, bool dark) const;

private:
    std::vector<PulseCircle> ripple_circles(double width, double ring, double hue, bool dark) const;

    std::vector<double> ripples_;
    double last_ = 0.0;
    double last_ripple_ = -1.0;
    double time_ = 0.0;
    VoiceSnapshot snapshot_;
};

// --- Dots: five dots that ripple with the voice, newest on the right ---

inline constexpr int kDotCount = 5;
inline constexpr double kDotSpacing = 20.0;
// How many history samples apart neighbouring dots read.
inline constexpr int kDotLag = 5;

struct Dot {
    PointD center;
    // The soft halo (halo_radius) under the solid dot (radius), both filled, no outline.
    double halo_radius = 0.0;
    Rgba halo_color;
    double radius = 0.0;
    Rgba color;
};
std::vector<Dot> dots_frame(const std::vector<double>& history, double width, double hue,
                            double grow, bool dark);
// They drop into a row and bounce in turn like a typing indicator.
std::vector<Dot> dots_finish(const VoiceSnapshot& snapshot, double width, double hue, double morph,
                             double clock, bool dark);

} // namespace fv
