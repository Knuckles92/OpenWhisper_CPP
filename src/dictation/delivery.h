#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "base/json.h"
#include "focus/clipboard_port.h"
#include "focus/synthetic_keys.h"

// Putting a finished transcript where the user is working: stage it on the clipboard, send the
// paste, put the user's clipboard back, or only copy it (services/runtime/transcription.py
// _apply_clipboard_and_paste, _paste_transcript, _copy_transcript).
namespace fv::dictation {

inline constexpr std::string_view READY_PASTED = "Ready (Pasted)";
inline constexpr std::string_view READY_PASTED_NO_RESTORE =
    "Ready (Pasted; clipboard restore unavailable)";
inline constexpr std::string_view PASTE_FAILED = "Transcription complete (paste failed)";
inline constexpr std::string_view COPY_FAILED = "Transcription complete (copy failed)";
inline constexpr std::string_view READY = "Ready";
inline constexpr std::string_view ADDED_TO_SCRATCHPAD = "Ready (Added to Scratchpad)";

// What delivery reaches; `clipboard` and `keys` must outlive every use.
// Affinity: UI actor (the clipboard is the GUI thread's).
struct DeliveryPort {
    focus::ClipboardPort* clipboard = nullptr;
    focus::SyntheticKeys* keys = nullptr;
    // ui_controller.insert_into_scratchpad: true when the focused Scratchpad took the text.
    // Empty (no Scratchpad until M2) takes nothing.
    std::function<bool(const std::string& text)> insert_into_scratchpad;
};

// Shows a delivery outcome on the status line.
using StatusReport = std::function<void(const std::string& status)>;

// Copies, and with auto_paste (or force_paste) pastes, after a successful clipboard write;
// every outcome goes to `report` with `status_suffix` after it. Only a paste consumes the
// clipboard snapshot the recording prefetched, so any other outcome discards it. True when the
// paste keystroke was sent. Reads auto_paste and copy_clipboard strictly from `settings`.
// Exceptions from the clipboard propagate. UI thread.
bool apply_clipboard_and_paste(const DeliveryPort& port, const Json& settings,
                               const std::string& transcript, const StatusReport& report,
                               std::string_view status_suffix = {}, const std::string& html = {},
                               bool force_paste = false);

// Stages `transcript` (with `html` when not empty; restore_clipboard decides whether the user's
// clipboard is kept) and sends Ctrl+V. On a failed paste the transcript stays on the clipboard;
// after a paste the user's clipboard comes back CLIPBOARD_RESTORE_DELAY later. True when the
// paste was sent. UI thread.
bool paste_transcript(const DeliveryPort& port, const Json& settings, const std::string& transcript,
                      const std::string& html, const StatusReport& report);

// Copies without pasting; a formatted snippet keeps its formatting. UI thread.
bool copy_transcript(const DeliveryPort& port, const std::string& transcript,
                     const std::string& html);

} // namespace fv::dictation
