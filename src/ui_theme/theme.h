#pragma once

#include <QColor>

namespace fv::ui {

class Palette;

// Every colour token of the palettes (all but "icon-suffix"): X(member, "token"). A "-rgb"
// triple becomes an opaque colour.
// clang-format off
#define FV_THEME_COLOURS(X) \
    X(bg, "bg") \
    X(surface, "surface") \
    X(surfaceHover, "surface-hover") \
    X(surfaceActive, "surface-active") \
    X(surfaceDisabled, "surface-disabled") \
    X(surfaceSunken, "surface-sunken") \
    X(codeOnSurface, "code-on-surface") \
    X(quoteOnSurface, "quote-on-surface") \
    X(codeOnBg, "code-on-bg") \
    X(quoteOnBg, "quote-on-bg") \
    X(border, "border") \
    X(borderSubtle, "border-subtle") \
    X(borderStrong, "border-strong") \
    X(borderHover, "border-hover") \
    X(text, "text") \
    X(textHeading, "text-heading") \
    X(textSoft, "text-soft") \
    X(textBody, "text-body") \
    X(textSecondary, "text-secondary") \
    X(textSecondaryStrong, "text-secondary-strong") \
    X(textTertiary, "text-tertiary") \
    X(textMuted, "text-muted") \
    X(textFaint, "text-faint") \
    X(knob, "knob") \
    X(knobBorder, "knob-border") \
    X(surfaceRgb, "surface-rgb") \
    X(surfaceHoverRgb, "surface-hover-rgb") \
    X(accent, "accent") \
    X(accentHover, "accent-hover") \
    X(accentPressed, "accent-pressed") \
    X(accentSoft, "accent-soft") \
    X(accentBorder, "accent-border") \
    X(accentCyan, "accent-cyan") \
    X(accentRgb, "accent-rgb") \
    X(accentCyanRgb, "accent-cyan-rgb") \
    X(accentTint, "accent-tint") \
    X(accentTintStrong, "accent-tint-strong") \
    X(accentTintBorder, "accent-tint-border") \
    X(accentTintBorderStrong, "accent-tint-border-strong") \
    X(accentMuted, "accent-muted") \
    X(onAccent, "on-accent") \
    X(onAccentRgb, "on-accent-rgb") \
    X(onTintRgb, "on-tint-rgb") \
    X(success, "success") \
    X(successHover, "success-hover") \
    X(successPressed, "success-pressed") \
    X(successText, "success-text") \
    X(successTextStrong, "success-text-strong") \
    X(successRgb, "success-rgb") \
    X(danger, "danger") \
    X(dangerHover, "danger-hover") \
    X(dangerPressed, "danger-pressed") \
    X(dangerText, "danger-text") \
    X(dangerTextSoft, "danger-text-soft") \
    X(dangerRgb, "danger-rgb") \
    X(warning, "warning") \
    X(warningHover, "warning-hover") \
    X(warningPressed, "warning-pressed") \
    X(warningText, "warning-text") \
    X(warningTextSoft, "warning-text-soft") \
    X(warningTextStrong, "warning-text-strong") \
    X(warningRgb, "warning-rgb") \
    X(warningTint, "warning-tint") \
    X(warningTintBorder, "warning-tint-border") \
    X(purple, "purple") \
    X(purpleText, "purple-text") \
    X(purpleRgb, "purple-rgb") \
    X(overlayRgb, "overlay-rgb") \
    X(inset, "inset") \
    X(insetSoft, "inset-soft") \
    X(shadowRgb, "shadow-rgb") \
    X(overlayBg, "overlay-bg") \
    X(overlayBorder, "overlay-border") \
    X(overlayText, "overlay-text") \
    X(splashBg, "splash-bg") \
    X(splashBorder, "splash-border") \
    X(slateBg, "slate-bg") \
    X(slateRail, "slate-rail") \
    X(slateRailHover, "slate-rail-hover") \
    X(slateSurface, "slate-surface") \
    X(slateSurfaceHover, "slate-surface-hover") \
    X(slateField, "slate-field") \
    X(slateRaised, "slate-raised") \
    X(slateDisabled, "slate-disabled") \
    X(slateBorder, "slate-border") \
    X(slateBorderSubtle, "slate-border-subtle") \
    X(slateBorderStrong, "slate-border-strong") \
    X(slateBorderHover, "slate-border-hover") \
    X(slateText, "slate-text") \
    X(slateText2, "slate-text-2") \
    X(slateText3, "slate-text-3") \
    X(slateText4, "slate-text-4") \
    X(slateTextDisabled, "slate-text-disabled") \
    X(slateText3Rgb, "slate-text-3-rgb") \
    X(slateCheckDisabled, "slate-check-disabled")
// clang-format on

// The palette's colours as QColor, for painted widgets (progress bars, switches, overlays,
// charts), so a paint reads a member instead of parsing a token string.
struct Theme {
#define FV_THEME_MEMBER(member, token) QColor member;
    FV_THEME_COLOURS(FV_THEME_MEMBER)
#undef FV_THEME_MEMBER

    static Theme fromPalette(const Palette& palette);
};

// The typed colours of currentPalette(). Affinity: GUI thread.
const Theme& currentTheme();

} // namespace fv::ui
