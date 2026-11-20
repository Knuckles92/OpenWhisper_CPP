#pragma once

#include <array>
#include <string_view>

#include "uimath/color.h"

namespace fv {

// The recording looks, in the order of the Settings segmented bar. The strings are what the
// setting stores.
inline constexpr std::string_view kLookRibbon = "ribbon";
inline constexpr std::string_view kLookBars = "bars";
inline constexpr std::string_view kLookWave = "wave";
inline constexpr std::string_view kLookAurora = "aurora";
inline constexpr std::string_view kLookPulse = "pulse";
inline constexpr std::string_view kLookDots = "dots";
inline constexpr std::string_view kLookBaitBall = "baitball";
inline constexpr std::string_view kLookFish = "fish";
inline constexpr std::string_view kLookGlow = "glow";
inline constexpr std::string_view kLookFizz = "fizz";
inline constexpr std::string_view kLookLegacy = "legacy";

inline constexpr std::array<std::string_view, 11> kLooks = {
    kLookRibbon,   kLookBars, kLookWave, kLookAurora, kLookPulse, kLookDots,
    kLookBaitBall, kLookFish, kLookGlow, kLookFizz,   kLookLegacy};

// config.RECORDING_OVERLAY_LOOK.
inline constexpr std::string_view kDefaultLook = kLookBars;

// The voice is drawn around this height, above the label row.
inline constexpr double kCenterY = 29.0;
// How long a look takes to morph from the recording into its work.
inline constexpr double kMorphS = 0.5;

// How a look draws small, in the live preview's header: squash keeps its lines and bars full
// size across the room and flattens them; shrink scales it evenly and draws it as wide as the
// room; fit scales the whole overlay's drawing evenly into it.
inline constexpr std::string_view kSmallSquash = "squash";
inline constexpr std::string_view kSmallShrink = "shrink";
inline constexpr std::string_view kSmallFit = "fit";

// A stored look, its current name, or the default for anything unknown. "live" is the ribbon,
// "orb" the pulse and "bubbles" the bait ball, as stored by earlier builds. Matching is exact:
// case and surrounding spaces count. The result views a constant, never value.
std::string_view normalize_look(std::string_view value);

// Smoothstep of t clamped to 0-1.
double smooth(double t);

// The particles' colour family at hue, tuned for the theme. dark picks the theme per call
// because it can change while the overlay is on screen. bright=false is the softer dark-theme
// variant; the light theme has one.
Rgba hue_color(double hue, double alpha = 255.0, bool bright = true, bool dark = true);

PointD lerp_point(PointD a, PointD b, double t);

} // namespace fv
