#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

// User-granted MCP mutations, separate from the read-only History API
// (services/agent_mcp/controls.py): what the token holder may change (capabilities), the
// preferences an agent may read and write (SETTING_CONTROLS), and retitling saved records.
namespace fv {
class SettingsStore;
}

namespace fv::agentapi {

// A public error that contains no credentials, paths or source text: "<code>: <message>"
// ("permission_denied: Enable this action in Settings > API & MCP.").
class ControlError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline constexpr std::string_view NEXT_OPERATION = "Applies to the next operation.";
inline constexpr std::string_view LIVE = "Applies immediately in the desktop app.";

// One preference an agent may see and, once granted, change.
// Affinity: value type.
struct SettingControl {
    std::string_view key;
    std::string_view label;
    std::string_view group;
    std::string_view kind = "boolean"; // boolean, integer, number or string
    Json choices = Json::array();      // empty: any value of the kind
    std::optional<std::int64_t> minimum;
    std::optional<std::int64_t> maximum;
    std::int64_t max_length = 0; // code points, for strings
    std::string_view effect = NEXT_OPERATION;
    bool resettable = false; // null removes the saved override

    // The value when it is valid for this control; throws ControlError "invalid_value: ...".
    // Only a resettable control accepts null. A bool is not an integer, 1.0 is not one either.
    Json validate(const Json& value) const;
    // The saved value, or the default when it is unset or invalid; the theme is
    // resolve_ui_theme's (Omarchy's own with none saved, on an Omarchy desktop).
    Json resolve(const Json& saved) const;
    // {key, label, group, type, effect, choices?, resettable?, minimum?, maximum?, max_length?}.
    Json describe() const;
};

// The 26 controls, in the order get_settings lists them. The custom cleanup prompt is left out
// on purpose: it is free-form instructions for the model that shapes what agent actions send.
const std::vector<SettingControl>& SETTING_CONTROLS();
const SettingControl* setting_control(std::string_view key);

// Capabilities that let the token holder change something, rather than read; reading live
// events is read access, like reading history.
inline constexpr std::array<std::string_view, 7> WRITE_CAPABILITIES = {
    "retitle_transcriptions", "retitle_meetings", "writable_settings", "run_actions",
    "control_recording",      "text_filters",     "agent_prompts"};

// The control keys whose mcp_writable_settings grant is exactly true; a malformed grant
// authorizes nothing.
std::set<std::string> writable_settings(const Json& saved);

// What the controls need from the app. `settings` null reads as no saved settings and refuses
// every change, as OpenWhisper's settings=None.
struct AgentControlsOptions {
    std::filesystem::path database;
    SettingsStore* settings = nullptr;
    std::function<bool()> enabled; // null: always on
    // ("settings" | "transcription" | "meeting", changes), after a committed write; what it
    // throws is logged by type and swallowed.
    std::function<void(std::string kind, Json changes)> on_change;
    // Retitles a saved meeting through the meeting runtime (it updates its own cached state);
    // may throw ControlError ("record_busy: ...").
    std::function<void(std::string id, std::string title)> meeting_renamer;
    bool allow_live_tools = true;
};

// Affinity: thread-safe; blocking (settings file, SQLite), never on the GUI thread.
class AgentControls {
public:
    explicit AgentControls(AgentControlsOptions options);

    // {retitle_transcriptions, retitle_meetings, settings_access, writable_settings (sorted, []
    // without settings access), live_events, run_actions, control_recording, text_filters,
    // agent_prompts, mcp_events}, read from the saved settings on every call.
    Json capabilities() const;
    // {"settings": [describe() + value, writable, inherited?]}; needs settings access.
    Json get_settings() const;
    // {updated, previous, restore}. The whole update is refused for one bad or ungranted key;
    // the grant check and the write are one SettingsStore::mutate.
    Json update_settings(const Json& changes);
    // kind "transcription" or "meeting": {id, title, previous_title}. Only local rows; a meeting
    // must be ended or failed, and its saved state's title changes with it.
    Json retitle(std::string_view kind, std::string_view record_id, std::string_view title);

private:
    Json saved() const;
    void require(const Json& saved, std::string_view permission) const;
    void changed(std::string_view kind, const Json& changes) const;

    AgentControlsOptions options_;
};

} // namespace fv::agentapi
