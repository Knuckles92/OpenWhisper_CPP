#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "base/json.h"

// The one-line values the Settings rail shows under the App pages and Recording
// (settings_dialog.py _render_rail_values). The other pages' values come with those pages.
// Affinity: thread-safe (pure functions; the no-argument appearance overload reads the
// desktop session once, as resolve_ui_theme does).
namespace fv::appcore {

// _general_rail_value: "Auto-paste on", else "Clipboard" when copying, else "Manual".
std::string general_rail_value(const Json& settings);

// "<Theme> · <Look>": UiTheme.LABELS[resolve_ui_theme] and RecordingOverlayLook.LABELS of the
// saved look. With no theme saved, an Omarchy desktop reads as "Omarchy desktop".
std::string appearance_rail_value(const Json& settings, bool omarchy_ui);
std::string appearance_rail_value(const Json& settings); // use_omarchy_ui() of this session

// The record shortcut's display text, or "Not set": `hotkeys` is the hotkey table the page holds
// (load_hotkey_settings when it holds none), `display` is format_hotkey_display for this
// platform. A missing or non-text record_toggle reads as "".
std::string hotkeys_rail_value(const Json& hotkeys,
                               const std::function<std::string(std::string_view)>& display);

// "Developer mode on" / "Developer mode off".
std::string developer_rail_value(const Json& settings);

// The Recording page's value: the saved microphone order (mic::rail_value).
std::string recording_rail_value(const Json& settings);

} // namespace fv::appcore
