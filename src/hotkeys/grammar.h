#pragma once

#include <array>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

// The shortcut strings settings store ("ctrl+alt+kp *"), as services/_hotkey_common.py,
// _hotkey_keyboard.py and _hotkey_pynput.py parse, rebuild and display them.
namespace fv::hotkeys {

// Whose key names a string uses: the Windows hook's (`keyboard` event names, modifiers
// ctrl/alt/shift/win) or pynput's (macOS and Linux, modifiers cmd/ctrl/alt/shift).
enum class Vocabulary { Windows, Pynput };

// This build's vocabulary, as services/hotkey_manager.py picks its backend by platform.
Vocabulary platform_vocabulary();

// Canonical modifier names (a frozenset in Python) and the main key, None for "".
struct ParsedHotkey {
    std::set<std::string> modifiers;
    std::optional<std::string> main_key;

    friend bool operator==(const ParsedHotkey&, const ParsedHotkey&) = default;
};

// Side mouse buttons a shortcut may use as its main key, with their display names.
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 2> kMouseKeys{{
    {"mouse4", "Mouse 4"},
    {"mouse5", "Mouse 5"},
}};

// Actions that fire once per press, in the order a press tries them. Record and Command Mode
// are not here: they also act on release.
inline constexpr std::array<std::string_view, 10> kPressActions{
    "meeting_toggle",    "cancel",         "minimize_tray",
    "scratchpad_toggle", "cycle_language", "paste_last_original",
    "route_record",      "walkthrough",    "screenshot",
    "paste_walkthrough",
};

// Families of shortcuts registered at run time, as "<namespace>:<id>" actions.
inline constexpr std::array<std::string_view, 4> kDynamicNamespaces{"profile", "transform",
                                                                    "action", "walkthrough_marks"};

bool is_mouse_key(std::optional<std::string_view> main_key);

// _hotkey_common.parse_hotkey_string: split on "+", strip and lowercase each part, drop empty
// parts; the last part is the main key (through `main_key_aliases`), the others modifiers through
// `modifier_aliases`, unknown ones ignored.
ParsedHotkey parse_hotkey(std::string_view hotkey, Vocabulary);
// _hotkey_common.format_hotkey_string: the modifiers in the vocabulary's order, then the main
// key, joined by "+".
std::string format_hotkey(const std::set<std::string>& modifiers,
                          const std::optional<std::string>& main_key, Vocabulary);
// format_hotkey_display. Windows: "Ctrl+Alt+Shift+Win+<key>", numpad keys as their symbol, one
// character upper-cased, other names title-cased, mouse keys "Mouse 4". Pynput: on macOS
// (`mac_symbols`) the modifier symbols with no separator and symbols for a few named keys; on
// Linux "Super/Ctrl/Alt/Shift" joined by "+", "_" shown as a space and "Kp x" as "Num x".
std::string format_hotkey_display(std::string_view hotkey, Vocabulary, bool mac_symbols = false);

} // namespace fv::hotkeys
