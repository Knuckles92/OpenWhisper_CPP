#include "settings/hotkeys.h"

#include <exception>
#include <string>

#include "base/log.h"
#include "settings/keys.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"services.settings"};

} // namespace

Json default_hotkeys() {
    // config.py:755-793, the branch for every platform but macOS. Command Mode, the Scratchpad,
    // language cycling, pasting the last original, routing by voice, walkthroughs and
    // screenshots have no default shortcut.
    Json hotkeys = Json::object();
    hotkeys["record_toggle"] = "kp *";
    hotkeys["cancel"] = "kp -";
    hotkeys["enable_disable"] = "ctrl+alt+kp *";
    hotkeys["minimize_tray"] = "ctrl+alt+m";
    hotkeys["meeting_toggle"] = "";
    for (const char* unbound :
         {"command_mode", "scratchpad_toggle", "cycle_language", "paste_last_original",
          "route_record", "walkthrough", "screenshot"}) {
        hotkeys[unbound] = "";
    }
    hotkeys["paste_walkthrough"] = "ctrl+shift+f12";
    return hotkeys;
}

Json load_hotkey_settings(const SettingsStore& store) {
    try {
        const auto settings = store.snapshot();
        const auto found = settings->find(keys::hotkeys.key());
        if (found != settings->end()) {
            return *found;
        }
        return default_hotkeys();
    } catch (const std::exception& error) {
        kLog.warning("Failed to load settings: {}; using the default hotkeys", error.what());
    }
    return default_hotkeys();
}

void save_hotkey_settings(SettingsStore& store, const Json& hotkeys) {
    try {
        Json patch = Json::object();
        patch[std::string(keys::hotkeys.key())] = hotkeys;
        store.update(patch);
        kLog.info("Hotkey settings saved successfully");
    } catch (const std::exception& error) {
        kLog.error("Failed to save settings: {}", error.what());
        throw;
    }
}

} // namespace fv
