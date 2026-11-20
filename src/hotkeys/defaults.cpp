#include "hotkeys/defaults.h"

namespace fv::hotkeys {

HotkeyList default_hotkeys(std::string_view platform) {
    const bool mac = platform == "darwin";
    HotkeyList hotkeys{
        {"record_toggle", mac ? "ctrl+alt+r" : "kp *"},
        {"cancel", mac ? "ctrl+alt+escape" : "kp -"},
        {"enable_disable", mac ? "ctrl+alt+shift+r" : "ctrl+alt+kp *"},
        {"minimize_tray", "ctrl+alt+m"},
        {"meeting_toggle", ""},
    };
    for (const char* unbound :
         {"command_mode", "scratchpad_toggle", "cycle_language", "paste_last_original",
          "route_record", "walkthrough", "screenshot"}) {
        hotkeys.emplace_back(unbound, "");
    }
    hotkeys.emplace_back("paste_walkthrough", "ctrl+shift+f12");
    return hotkeys;
}

} // namespace fv::hotkeys
