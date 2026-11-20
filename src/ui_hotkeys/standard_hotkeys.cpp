#include "ui_hotkeys/standard_hotkeys.h"

#include <utility>

#include "base/identity.h"
#include "hotkeys/conflicts.h"

namespace fv::standard_hotkeys {

namespace {

Item group(std::string title) {
    return {Item::Kind::Group, "", std::move(title), "", false};
}

Item row(std::string key, std::string title, std::string description, bool required = false) {
    return {Item::Kind::Row, std::move(key), std::move(title), std::move(description), required};
}

} // namespace

std::vector<Item> walkthroughRows() {
    return {
        row("paste_walkthrough", "Paste latest walkthrough",
            "Paste the newest completed walkthrough's PDF, then its agent instructions. Click "
            "your chat's message box first. The message stays unsent."),
        row("walkthrough", "Walkthrough",
            "Talk while you take screenshots. Press again to finish the document and deliver it "
            "using your walkthrough copy settings."),
        row("screenshot", "Screenshot",
            "Freeze the screen and mark it up. During a walkthrough it joins the walkthrough; "
            "otherwise it's copied."),
    };
}

std::vector<Item> items() {
    const std::string product(identity().product);
    std::vector<Item> all{
        group("Recording"),
        row("record_toggle", "Record hotkey", recordHotkeyDescription("toggle"), true),
        {Item::Kind::RecordModeCard, "", "", "", false},
        row("command_mode", "Command Mode",
            "Say how to rewrite the selected text, or what to write at the cursor. Works like "
            "the record hotkey."),
        row("route_record", "Route by voice",
            "Record, then send your words to the action they name, such as “Ask "
            "Claude…”. Turn on Route by voice on the Actions page."),
        row("cancel", "Cancel", "Discard an active recording or interrupt transcription.", true),
        row("meeting_toggle", "Meeting Mode",
            "Start or end Meeting Mode. Leave empty to disable this shortcut."),
        group("Dictation tools"),
        row("scratchpad_toggle", "Scratchpad", "Show or hide the floating Scratchpad."),
        row("cycle_language", "Switch language", "Move to the next language you dictate in."),
        row("paste_last_original", "Paste original of last dictation",
            "Paste your last dictation as you said it, before AI cleanup."),
        group("Walkthroughs"),
    };
    for (Item& item : walkthroughRows()) {
        all.push_back(std::move(item));
    }
    all.push_back(group(product));
    all.push_back(row("enable_disable", "Enable or disable hotkeys",
                      "Temporarily enable or disable every " + product + " hotkey.", true));
    all.push_back(
        row("minimize_tray", "Minimize to tray", "Hide the main window in the system tray.", true));
    return all;
}

std::string recordHotkeyDescription(std::string_view mode, bool handsFree) {
    if (mode == "push_hold") {
        if (handsFree) {
            return "Hold to record; release to stop and transcribe. Double-tap to record "
                   "hands-free.";
        }
        return "Hold to record; release to stop and transcribe. Quick taps are canceled.";
    }
    return "Start recording when idle; stop and transcribe while recording.";
}

std::string label(std::string_view action) {
    for (const auto& [key, text] : hotkeys::standard_labels()) {
        if (key == action) {
            return std::string(text);
        }
    }
    return "Shortcut";
}

} // namespace fv::standard_hotkeys
