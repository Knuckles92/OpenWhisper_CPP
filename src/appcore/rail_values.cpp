#include "appcore/rail_values.h"

#include <format>

#include "appcore/mic_priority.h"
#include "base/desktop_session.h"
#include "settings/enums.h"
#include "settings/keys.h"

namespace fv::appcore {

namespace {

std::string label_of(enums::EnumId id, const std::string& value) {
    return enums::label(id, Json(value)).value_or(value);
}

} // namespace

std::string general_rail_value(const Json& settings) {
    if (keys::auto_paste.read(settings)) {
        return "Auto-paste on";
    }
    if (keys::copy_clipboard.read(settings)) {
        return "Clipboard";
    }
    return "Manual";
}

std::string appearance_rail_value(const Json& settings, bool omarchy_ui) {
    const bool theme_saved =
        settings.is_object() && settings.contains(std::string(keys::ui_theme.key()));
    const std::string theme =
        !theme_saved && omarchy_ui ? std::string("omarchy") : keys::ui_theme.read(settings);
    return std::format(
        "{} · {}", label_of(enums::EnumId::UiTheme, theme),
        label_of(enums::EnumId::RecordingOverlayLook, keys::recording_overlay_look.read(settings)));
}

std::string appearance_rail_value(const Json& settings) {
    return appearance_rail_value(settings, use_omarchy_ui());
}

std::string hotkeys_rail_value(const Json& hotkeys,
                               const std::function<std::string(std::string_view)>& display) {
    std::string raw;
    if (hotkeys.is_object()) {
        const auto found = hotkeys.find("record_toggle");
        if (found != hotkeys.end() && found->is_string()) {
            raw = found->get<std::string>();
        }
    }
    std::string shown = display(raw);
    return shown.empty() ? std::string("Not set") : shown;
}

std::string developer_rail_value(const Json& settings) {
    return keys::developer_mode.read(settings) ? "Developer mode on" : "Developer mode off";
}

std::string recording_rail_value(const Json& settings) {
    return mic::rail_value(settings);
}

} // namespace fv::appcore
