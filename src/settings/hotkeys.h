#pragma once

#include "settings/setting.h"
#include "settings/store.h"

namespace fv {

// config.DEFAULT_HOTKEYS on Windows, in its key order: record_toggle "kp *", cancel "kp -",
// enable_disable "ctrl+alt+kp *", minimize_tray "ctrl+alt+m", meeting_toggle "", the seven
// unbound actions "", paste_walkthrough "ctrl+shift+f12". A fresh copy.
Json default_hotkeys();

// The saved `hotkeys` value whole, or a copy of the defaults when none is saved: never a
// per-key merge, and not validated (an old file's partial table comes back as it is).
Json load_hotkey_settings(const SettingsStore&);

// Saves `hotkeys` as the whole `hotkeys` value.
void save_hotkey_settings(SettingsStore&, const Json& hotkeys);

} // namespace fv
