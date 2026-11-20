#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/platform_info.h"

namespace fv::hotkeys {

// Hotkeys by action, in config.DEFAULT_HOTKEYS' key order.
using HotkeyList = std::vector<std::pair<std::string, std::string>>;

// config.DEFAULT_HOTKEYS for a platform named as sys.platform names it. macOS gets Control+Option
// combinations (they avoid its system shortcuts, and laptops have no numpad); every other
// platform the numpad. Command Mode, the Scratchpad, language cycling, pasting the last original,
// routing by voice, walkthroughs and screenshots have no default shortcut anywhere.
HotkeyList default_hotkeys(std::string_view platform = python_platform());

// config.HOTKEY_DEBOUNCE_MS: the record toggle and each family item ignore a press this soon
// after the last one that fired.
inline constexpr int kHotkeyDebounceMs = 300;

} // namespace fv::hotkeys
