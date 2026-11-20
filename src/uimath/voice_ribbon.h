#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

#include "uimath/color.h"

namespace fv {

// How far the particles' launch line sits above the overlay's bottom, as the legacy emitter did.
inline constexpr double kBaselineFromBottom = 30.0;
// The loudest speech lifts the ribbon this many pixels.
inline constexpr double kRibbonRise = 22.0;
inline constexpr double kRibbonHistoryS = 2.2;
// About 45 samples a second: finer than a syllable.
inline constexpr std::size_t kRibbonSamples = 100;
// A level meter's ballistics: a rise shows at once; a fall decays with this time constant,
// quick enough that syllables stay apart.
inline constexpr double kRibbonReleaseS = 0.06;
// The ribbon's margin at each side of the overlay.
inline constexpr double kRibbonEdge = 14.0;
// How strongly loudness drives particle emission in the live looks; the legacy look uses the
// particle config's own value.
inline constexpr double kLiveAudioResponse = 2.6;

struct GradientStop {
    double position; // 0-1 along the gradient
    Rgba color;
};

// What VoiceRibbon::draw paints: a soft band under a bright edge, in the particles' hues.
struct RibbonShape {
    // The ribbon's top edge, 81 points from the left margin to the right one. The band is the
    // polygon from (top.front().x, bottom_y) up through these to (top.back().x, bottom_y).
    std::vector<PointD> top;
    double bottom_y = 0.0;
    // Both gradients run horizontally from gradient_left to gradient_right at y 0.
    double gradient_left = 0.0;
    double gradient_right = 0.0;
    std::vector<GradientStop> fill;
    std::vector<GradientStop> edge;
    // The edge line's width; round caps.
    double edge_pen_width = 1.6;
};

// The last kRibbonHistoryS seconds of loudness, newest on the right, with a level meter's
// ballistics. Affinity: GUI thread (the overlay's frame timer and level slot).
class VoiceRibbon {
public:
    VoiceRibbon() { reset(); }

    void reset();

    // Takes the microphone's RMS level (0-1 of full scale). Several levels can arrive between
    // frames; the loudest one counts.
    void hear(double level);

    // One frame of dt seconds.
    void advance(double dt);

    // The meter's reading of the latest level, before the ballistics.
    double loudness() const { return loudness_; }
    // The meter's reading now: the live edge of the ribbon.
    double shown() const { return shown_; }

    // Heights 0-1, oldest first.
    std::vector<double> history() const;

    // The ribbon's 0-1 height at x across an overlay width wide.
    double height_at(double x, double width) const;

    // Where a particle leaves from at x: the ribbon's top edge.
    double crest(double x, double width, double baseline) const;

    // The band and edge for the overlay's width, with its baseline y, the particles' hue and
    // the grow-in factor (0-1); nullopt while grow <= 0. dark selects the theme's colours.
    std::optional<RibbonShape> shape(double width, double baseline, double hue, double grow,
                                     bool dark) const;

private:
    double loudness_ = 0.0;
    double shown_ = 0.0;
    double peak_ = 0.0;
    double slot_peak_ = 0.0;
    std::deque<double> history_;
    double carry_ = 0.0;
};

} // namespace fv
