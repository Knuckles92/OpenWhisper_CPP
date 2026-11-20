#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <QString>

#include "base/json.h"

class QApplication;

namespace fv::ui {

class ThemeManager;

// UiTheme values (settings key ui_theme). "omarchy" is a valid saved value, but its desktop
// palette is not ported: it resolves like any other non-light choice, to dark.
inline constexpr char kUiThemeDark[] = "dark";
inline constexpr char kUiThemeLight[] = "light";
inline constexpr char kUiThemeSystem[] = "system";
inline constexpr char kUiThemeOmarchy[] = "omarchy";

// Every "font-size: N(px|pt)" in stylesheet multiplied by scale; other lengths stay, so padding
// and control sizes keep their designed values. Integers become max(1, round(N * scale)) with
// Python's half-to-even round; decimals keep one decimal, trailing zeros and dot stripped. A
// scale within 1e-6 of 1.0 returns the input unchanged.
std::string scaleQssFonts(std::string_view stylesheet, double scale);
QString scaleQssFonts(const QString& stylesheet, double scale);

// A designed sheet as Qt receives it: currentPalette()'s tokens substituted, then font sizes
// scaled (by currentUiFontScale() when scale is not given). An unknown token is logged and left
// in place.
QString resolveStylesheet(const QString& source, std::optional<double> scale = std::nullopt);

// The active scale as a multiplier of the designed theme (1.0 = 100 %), and as a percent.
double currentUiFontScale();
int currentUiFontScalePercent();
// A designed pixel size at the active scale: round(px * scale), half to even. For widgets that
// carry pixel sizes (switch, help card, studio stage).
int scaled(int px);

// Applies percent to the application stylesheet (when a ThemeManager is known), the app font
// (Segoe UI, max(6, round(10 * scale)) pt) and every widget-local sheet, re-resolved from its
// source so scales never compound. The first form remembers manager for later calls.
void applyUiFontScale(int percent, QApplication& app, ThemeManager& manager);
void applyUiFontScale(int percent, QApplication& app);

// The palette name for a UiTheme preference: "light" is light, anything but "system" is dark,
// and "system" reads the platform colour scheme (light only when it is Light).
QString resolveThemePreference(const QString& preference, const QApplication* app);

// The preference last given to applyUiTheme ("dark" until then).
QString currentUiThemePreference();

// Applies a UiTheme preference: switches manager's palette, pushes the QPalette, and, when the
// palette changed, restyles the application and every widget-local sheet. An explicit choice
// pins the platform colour scheme so the window frame matches; "system" unsets it instead,
// because a pinned scheme replaces the one it reads and silences colorSchemeChanged. Returns
// true when the palette changed.
bool applyUiTheme(const QString& preference, QApplication& app, ThemeManager& manager);

// Re-applies "system" whenever the platform colour scheme changes while that is the preference,
// so "Match system" follows the OS live. The connection lasts as long as manager.
void followPlatformColorScheme(QApplication& app, ThemeManager& manager);

// The saved ui_theme of a settings snapshot: one of dark, light, system, omarchy, else the
// default "dark".
QString resolveUiTheme(const Json& settings);

} // namespace fv::ui
