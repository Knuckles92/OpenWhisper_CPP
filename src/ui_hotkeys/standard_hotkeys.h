#pragma once

#include <string>
#include <string_view>
#include <vector>

// The standard shortcuts' rows on Settings > Hotkeys, as settings_dialog.py's
// _build_hotkeys_page and _add_walkthrough_shortcut_rows lay them out.
namespace fv::standard_hotkeys {

// One item of the page, in page order: a group heading, a shortcut row, or the record-mode
// card.
struct Item {
    enum class Kind { Group, Row, RecordModeCard };
    Kind kind;
    std::string key;   // Row: the action ("record_toggle")
    std::string title; // Group: the heading; Row: the row's name
    std::string description;
    // Not optional: Clear stays disabled, so the shortcut can be changed but never left empty.
    bool required = false;
};

// The page's items. The record row's description is recordHotkeyDescription("toggle"); the
// last group is named after identity().product, and so is the Enable or disable row's text.
std::vector<Item> items();

// _add_walkthrough_shortcut_rows: the three rows the Hotkeys page lists under "Walkthroughs" and
// the Walkthroughs page lists under "Shortcuts", in that order (paste_walkthrough, walkthrough,
// screenshot). All three are optional.
std::vector<Item> walkthroughRows();

// _record_hotkey_description: what the record row says for a recording_trigger_mode value.
std::string recordHotkeyDescription(std::string_view mode, bool handsFree = false);

// _HOTKEY_LABELS.get(action, "Shortcut"): how a message names a standard action (it is
// hotkeys::standard_labels()).
std::string label(std::string_view action);

} // namespace fv::standard_hotkeys
