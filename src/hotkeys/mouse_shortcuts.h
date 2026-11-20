#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "hotkeys/grammar.h"

// What the shortcut fields say about side buttons and Escape (ui_qt/widgets/hotkey_capture.py,
// services/_hotkey_keyboard.py mouse_shortcuts_supported).
namespace fv::hotkeys {

// Whether a side button can be a shortcut here: true with the Windows hook, false elsewhere.
bool mouse_shortcuts_supported();

// Whether `name` is Escape ("esc" or "escape", any case, surrounding whitespace ignored). Esc
// alone cancels a capture; with modifiers it is a shortcut like any other.
bool is_escape_key(std::optional<std::string_view> name);

// What binding a side button changes about it in other apps, or "" when nothing: with the
// Windows hook "Mouse 4 won't work as Back in other apps while it's a shortcut."; with pynput's
// backend a side button without modifiers "Mouse 4 also still goes Back in the app you're
// using." (X11 cannot keep the click from the focused app).
std::string mouse_shortcut_note(std::string_view hotkey,
                                Vocabulary vocabulary = platform_vocabulary());

} // namespace fv::hotkeys
