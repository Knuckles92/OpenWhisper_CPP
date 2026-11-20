#pragma once

#include <QString>

// History actions reached from outside the History sidebar: the tray's "Copy original of last
// dictation" and the paste-original shortcut (ui_qt/history_actions.py).
namespace fv {

class HistoryStore;

// What the actions ask of the UI they report to (the UI controller, M1.16), the duck-typed
// `ui` of history_actions.py. Affinity: any thread the caller runs the action on; the
// implementation brings what it shows to the GUI thread.
class HistoryActionsUi {
public:
    virtual ~HistoryActionsUi() = default;
    virtual void setStatus(const QString& status) = 0;
    // A notice near the pointer, since these run from other apps; done marks a finished action.
    // Python's ui offered it optionally: the default shows nothing.
    virtual void showNotice(const QString& text, bool done);
    // Whether pasteTextNow exists (Python's on_paste_text_now can be missing).
    virtual bool canPasteNow() const { return true; }
    // Pastes text at the caret now; false when it did not (the runtime already said why).
    virtual bool pasteTextNow(const QString& text) = 0;
    virtual void refreshHistory() = 0;
    // false when the clipboard could not be written.
    virtual bool copyToClipboard(const QString& text) = 0;
};

namespace history_actions {
inline constexpr char kNothingYet[] = "Nothing to paste yet";
inline constexpr char kNothingToCopy[] = "Nothing to copy yet";
inline constexpr char kNotEdited[] = "AI cleanup didn't change your last dictation";
inline constexpr char kPasted[] = "Pasted the original of your last dictation";
inline constexpr char kUnavailable[] = "Pasting isn't available right now";
inline constexpr char kCopied[] = "Copied the original of your last dictation — paste it with "
                                  "Ctrl+V";
inline constexpr char kCopyFailed[] = "Couldn't copy the original of your last dictation";
inline constexpr char kReadFailed[] = "Couldn't read your last dictation";
} // namespace history_actions

// Pastes the last dictation's text from before AI cleanup at the caret, the shortcut's undo for
// a cleanup that already went into the app, and marks the entry as original in History. Only
// the newest dictation counts, so pressing it twice never reaches back to an older one. Blocks
// on the History database: never on the GUI thread (the hotkey runtime calls it on its thread
// once the shortcut's modifiers are released).
void paste_last_original(HistoryActionsUi& ui, HistoryStore& history);

// Puts the last dictation's text from before AI cleanup on the clipboard: the tray's version,
// since from a menu there is no telling which window a paste would reach. Nothing was pasted,
// so History keeps showing the version it showed. Blocks on the History database: never on the
// GUI thread.
void copy_last_original(HistoryActionsUi& ui, HistoryStore& history);

} // namespace fv
