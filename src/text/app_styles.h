#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "focus/catalog.h"
#include "focus/snapshot.h"
#include "settings/install.h"

// Writing styles per kind of app: the tone AI cleanup uses where you dictate
// (services/app_styles.py). Styles only change output while AI cleanup is on, and an explicit
// cleanup profile always wins over a style. The style block names a kind of writing, never the
// app, so styles alone send nothing about where you dictate to the cleanup provider.
// `settings` is a settings mapping; anything else reads as {}. Mutators throw py::ValueError and
// edit the mapping they are given, for SettingsStore::mutate. Every function is thread-safe.
namespace fv::text::app_styles {

// AppCategory.
inline constexpr std::string_view EMAIL = focus::catalog::EMAIL;
inline constexpr std::string_view WORK = focus::catalog::WORK;
inline constexpr std::string_view PERSONAL = focus::catalog::PERSONAL;
inline constexpr std::string_view OTHER = focus::catalog::OTHER;
inline constexpr std::array<std::string_view, 4> CATEGORIES{EMAIL, WORK, PERSONAL, OTHER};

// Tone.
inline constexpr std::string_view FORMAL = "formal";
inline constexpr std::string_view CASUAL = "casual";
inline constexpr std::string_view VERY_CASUAL = "very_casual";
inline constexpr std::array<std::string_view, 3> TONES{FORMAL, CASUAL, VERY_CASUAL};

// Surface.
inline constexpr std::string_view SURFACE_TEXT = focus::catalog::TEXT;
inline constexpr std::string_view SURFACE_CODE = focus::catalog::CODE;
inline constexpr std::string_view SURFACE_TERMINAL = focus::catalog::TERMINAL;

// Seeded once on a brand-new install (seed_new_install_settings). Existing installs have no
// app_style_tones key, which reads as every category Formal: today's output.
inline constexpr const auto& NEW_INSTALL_TONES = kNewInstallAppStyleTones;

struct AppStyle {
    std::string category;
    std::string tone;
    std::string app_name;
    std::string surface = std::string(SURFACE_TEXT);

    friend bool operator==(const AppStyle&, const AppStyle&) = default;
};

// {category: tone} for every category, in CATEGORIES order; an invalid or missing tone is
// Formal (an existing install has no app_style_tones and reads Formal everywhere).
Json resolve_tones(const Json& settings);

// The user's valid (match, category) choices; the first one per app wins.
std::vector<focus::catalog::Override> resolve_overrides(const Json& settings);

// The category of the app in `snapshot`, styles on or off; "" when unknown or our own window.
// The same answer as dictation::category_for.
std::string category_for(const std::optional<focus::FocusSnapshot>& snapshot, const Json& settings);

// The style for the app in `snapshot`, or nullopt when styles are off, there is no identity,
// it is our own window or the app is unknown.
std::optional<AppStyle> style_for(const std::optional<focus::FocusSnapshot>& snapshot,
                                  const Json& settings);

// The cleanup-prompt block asking for `style`'s tone, or "". Formal is the cleanup prompt's own
// voice, so it adds nothing; terminals and code editors get their rule whatever the tone.
std::string prompt_block(const std::optional<AppStyle>& style);

// Stores one category's tone. Writing any tone stores all four, so a category left unset stays
// Formal rather than picking up a later default. Returns whether it changed.
bool set_tone(Json& settings, std::string_view category, std::string_view tone);

// Puts the app or site named `match` (stripped, cut to MAX_MATCH_CHARS) in `category`, newest
// first: an earlier choice for the same name (ignoring case) is replaced, and a choice equal to
// the built-in category is dropped instead of stored. "Choose an app and a style" otherwise.
void set_override(Json& settings, std::string_view match, std::string_view category);

// Forgets the user's choice for `match` (ignoring case and surrounding space).
void remove_override(Json& settings, std::string_view match);

} // namespace fv::text::app_styles
