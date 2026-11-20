#pragma once

#include <array>
#include <span>
#include <string_view>
#include <utility>

// The Settings window's destination keys and rail layout, from OpenWhisper's
// ui_qt/dialogs/settings_destinations.py and settings_dialog.py (_CHAPTERS). Callers deep-link
// into one destination by key (CLI, deep links, open_settings_destination), so the keys never
// depend on rail order; the rail order is the order Settings pages are listed in.

namespace fv::destinations {

inline constexpr std::string_view overview = "overview";
// Route to a Home card, not to a page of their own.
inline constexpr std::string_view basic_dictation = "basic_dictation";
inline constexpr std::string_view basic_meetings = "basic_meetings";
inline constexpr std::string_view basic_app = "basic_app";

// Dictation
inline constexpr std::string_view voice_model = "voice_model";
inline constexpr std::string_view remote_engine = "remote_engine";
inline constexpr std::string_view recording = "recording";
inline constexpr std::string_view cleanup = "cleanup";
inline constexpr std::string_view walkthroughs = "walkthroughs";

// Personalize
inline constexpr std::string_view dictionary = "dictionary";
inline constexpr std::string_view snippets = "snippets";
inline constexpr std::string_view styles = "styles";
inline constexpr std::string_view cleanup_rules = "cleanup_rules";
inline constexpr std::string_view cleanup_profiles = "cleanup_profiles";
inline constexpr std::string_view commands = "commands";

// Automate; API & MCP is mcp below.
inline constexpr std::string_view actions = "actions";
inline constexpr std::string_view hooks = "hooks";

// Meeting Mode
inline constexpr std::string_view meeting_voice = "meeting_voice";
inline constexpr std::string_view meeting_intelligence = "meeting_intelligence";
inline constexpr std::string_view meeting_fast = "meeting_fast";
inline constexpr std::string_view meeting_after = "meeting_after";
inline constexpr std::string_view meeting_dashboard = "meeting_dashboard";

// Models & storage
inline constexpr std::string_view downloads = "downloads";
inline constexpr std::string_view runtime = "runtime";
inline constexpr std::string_view backup = "backup";

// App
inline constexpr std::string_view general = "general";
inline constexpr std::string_view appearance = "appearance";
inline constexpr std::string_view hotkeys = "hotkeys";
inline constexpr std::string_view api_keys = "api_keys";
inline constexpr std::string_view mcp = "mcp";
inline constexpr std::string_view advanced = "advanced";

// Names the retired Model Manager and Downloads windows accepted.
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 6> legacy_aliases{{
    {"ondemand", voice_model},
    {"text", cleanup},
    {"meeting", meeting_voice},
    {"library", downloads},
    {"voice", downloads},
    {"engine_downloads", downloads},
}};

// Maps a destination key or legacy alias to a destination key; anything else is returned as is.
constexpr std::string_view resolve_destination(std::string_view name) {
    for (const auto& [alias, key] : legacy_aliases) {
        if (alias == name) {
            return key;
        }
    }
    return name;
}

// A page of the rail: its key, rail name and icon (a Tabler SVG of the theme's assets).
struct RailPage {
    std::string_view key;
    std::string_view name;
    std::string_view icon;
};

// A collapsible chapter of the rail, below Home.
struct RailChapter {
    std::string_view title;
    std::string_view icon;
    std::span<const RailPage> pages;
};

inline constexpr RailPage home{overview, "Home", "home-blue.svg"};

inline constexpr std::array<RailPage, 5> dictation_pages{{
    {recording, "Recording", "microphone-blue.svg"},
    {voice_model, "Voice model", "microphone-blue.svg"},
    {cleanup, "AI cleanup", "stack-purple.svg"},
    {remote_engine, "Remote engine", "server-blue.svg"},
    {walkthroughs, "Walkthroughs", "file-text-blue.svg"},
}};
inline constexpr std::array<RailPage, 6> personalize_pages{{
    {dictionary, "Dictionary", "book-blue.svg"},
    {snippets, "Snippets", "text-plus-green.svg"},
    {styles, "Apps & styles", "palette-purple.svg"},
    {cleanup_rules, "Learned rules", "stack-slate.svg"},
    {cleanup_profiles, "Profiles", "typography-blue.svg"},
    {commands, "Commands", "wand-purple.svg"},
}};
inline constexpr std::array<RailPage, 3> automate_pages{{
    {actions, "Actions", "bolt-green.svg"},
    {hooks, "Hooks", "webhook-blue.svg"},
    {mcp, "API & MCP", "server-blue.svg"},
}};
inline constexpr std::array<RailPage, 5> meeting_pages{{
    {meeting_voice, "Voice & speakers", "microphone-blue.svg"},
    {meeting_intelligence, "Intelligence", "stack-purple.svg"},
    {meeting_fast, "Fast judgments", "bolt-green.svg"},
    {meeting_after, "After the meeting", "check-green.svg"},
    {meeting_dashboard, "Dashboard", "box-blue.svg"},
}};
inline constexpr std::array<RailPage, 3> storage_pages{{
    {downloads, "Downloads", "download-blue.svg"},
    {runtime, "Runtime", "cpu-blue.svg"},
    {backup, "Backup & restore", "box-blue.svg"},
}};
inline constexpr std::array<RailPage, 5> app_pages{{
    {general, "General", "bolt-green.svg"},
    {appearance, "Appearance", "palette-purple.svg"},
    {hotkeys, "Hotkeys", "keyboard-green.svg"},
    {api_keys, "API keys", "key-blue.svg"},
    {advanced, "Developer options", "box-blue.svg"},
}};

// The rail below Home, top to bottom.
inline constexpr std::array<RailChapter, 6> rail{{
    {"Dictation", "microphone-blue.svg", dictation_pages},
    {"Personalize", "wand-purple.svg", personalize_pages},
    {"Automate", "bolt-green.svg", automate_pages},
    {"Meeting Mode", "users-amber.svg", meeting_pages},
    {"Models & storage", "download-blue.svg", storage_pages},
    {"App", "adjustments-slate.svg", app_pages},
}};

} // namespace fv::destinations
