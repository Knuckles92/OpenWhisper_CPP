#include "hotkeys/mouse_shortcuts.h"

#include <format>

#include "pyport/text.h"

namespace fv::hotkeys {

bool is_escape_key(std::optional<std::string_view> name) {
    if (!name || name->empty()) {
        return false;
    }
    const std::string key = py::lower(py::strip(*name));
    return key == "esc" || key == "escape";
}

std::string mouse_shortcut_note(std::string_view hotkey, Vocabulary vocabulary) {
    const ParsedHotkey parsed = parse_hotkey(hotkey, vocabulary);
    if (!is_mouse_key(parsed.main_key)) {
        return "";
    }
    const bool back = *parsed.main_key == "mouse4";
    const std::string_view name = back ? "Mouse 4" : "Mouse 5";
    const std::string_view action = back ? "Back" : "Forward";
    if (vocabulary == Vocabulary::Windows) {
        return std::format("{} won't work as {} in other apps while it's a shortcut.", name,
                           action);
    }
    if (parsed.modifiers.empty()) {
        return std::format("{} also still goes {} in the app you're using.", name, action);
    }
    return "";
}

} // namespace fv::hotkeys
