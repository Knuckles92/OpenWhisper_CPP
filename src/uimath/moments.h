#pragma once

#include <array>
#include <random>
#include <span>
#include <string_view>
#include <vector>

#include "uimath/color.h"
#include "uimath/easing.h"
#include "uimath/particles.h"

namespace fv {

// The overlay's one-shot moments: a finished command, a toggle, a copy. Each plays the same
// choreography: a lead-in, then a disc popping in the moment's tone with two rings rolling out
// and the pill's edge flashing, a glyph that draws itself, confetti, and a label rising under a
// glint. This header holds the timings, the tables and the maths; painting is the overlay's.

inline constexpr double kMomentRadius = 13.0;
inline constexpr double kPopS = 0.24;
inline constexpr double kRingS = 0.6;
inline constexpr double kGlyphDelayS = 0.1;
inline constexpr double kGlyphS = 0.26;
inline constexpr double kLabelDelayS = 0.08;
inline constexpr double kLabelS = 0.3;
inline constexpr double kGlintDelayS = 0.25;
inline constexpr double kGlintS = 0.8;
// The fade of a moment's contents before the overlay hides (moments.FADE_S).
inline constexpr double kFadeS = kMomentFadeS;

// A sparkle a finished rewrite gathers in, as fractions of the overlay's size.
struct Sparkle {
    double x_frac;
    double y_frac;
    double base_size;
};
inline constexpr std::array<Sparkle, 5> kSparkles = {{
    {0.50, 0.42, 11.0},
    {0.37, 0.28, 6.0},
    {0.64, 0.30, 7.5},
    {0.41, 0.58, 5.0},
    {0.61, 0.55, 6.5},
}};

// Small sparkles left twinkling beside an AI edit's check.
struct AfterglowSpark {
    double dx;
    double dy;
    double size;
    double phase;
};
inline constexpr std::array<AfterglowSpark, 3> kAfterglow = {{
    {23.0, -11.0, 4.5, 0.0},
    {-22.0, -8.0, 3.5, 2.1},
    {19.0, 11.0, 3.0, 4.2},
}};

// A glyph's strokes around the disc's centre, drawn in order, and its pen width.
struct Glyph {
    std::string_view name;
    std::vector<std::vector<PointD>> strokes;
    double pen_width;
};
// "check", "cross", "bang" or "clip"; throws std::out_of_range for another name.
const Glyph& glyph(std::string_view name);

// A glyph drawn part of the way: each polyline is one subpath (QPainterPath moveTo, then
// lineTo for each vertex).
struct GlyphPath {
    std::vector<std::vector<PointD>> strokes;
    // The summed length of every segment.
    double length() const;
};

// The first `amount` (0-1) of the glyph's length, centred on center, as if being drawn.
GlyphPath glyph_path(std::string_view name, PointD center, double amount);

struct HueRange {
    double low;
    double high;
};

// How one moment plays.
struct Moment {
    // Palette role of the disc, the second ring and the edge flash.
    std::string_view tone;
    // Role of the first ring, the lead-in and half the confetti.
    std::string_view partner;
    // Key into the glyph table.
    std::string_view glyph;
    // "sparkles" (the rewrite's), "converge" (dots spiral in), "collapse" (what was on screen
    // implodes) or "".
    std::string_view lead_in;
    // When the disc pops, in seconds, after the lead-in.
    double pop_at;
    // Confetti hue ranges for the tone's half and the partner's half.
    std::array<HueRange, 2> hues;
    // Role of the glint that passes over the label.
    std::string_view glint;
    // Gravity on the confetti; a switch turning off lets it drop.
    double fall = 0.0;
    // Leave the rewrite's sparkles twinkling beside the glyph.
    bool afterglow = false;
};

struct NamedMoment {
    std::string_view key;
    Moment moment;
};
// MOMENTS: "done", "enabled", "disabled" and "copied", in the order the Python dict holds them.
std::span<const NamedMoment> moments();
// A moment of moments() by key; nullptr for another key.
const Moment* find_moment(std::string_view key);

// Canceled is the one moment with weight: the recording collapses into a bright core, the disc
// slams in from oversize, the whole pill flashes red and a burst of glowing bubbles fills it.
const Moment& canceled_moment();

// Confetti in the moment's two colours, flattened to the pill's shape (count particles, the
// hue alternating between the two ranges).
std::vector<Particle> burst(const Moment& moment, double cx, double cy, std::mt19937& rng,
                            int count = 40);
// One tick of confetti: a quick burst that fades, not a drift. Particles whose life is spent
// are removed, the order of the rest kept.
void advance_confetti(std::vector<Particle>& particles, double dt, double fall);

// Big glowing bubbles that fill the pill: the original Canceled's look.
std::vector<Particle> bubble_burst(const Moment& moment, double cx, double cy, std::mt19937& rng,
                                   int count = 72);
// Bubbles jostle and drift out; they outlive confetti.
void advance_bubbles(std::vector<Particle>& particles, double dt, std::mt19937& rng);

} // namespace fv
