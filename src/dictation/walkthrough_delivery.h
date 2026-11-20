#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "focus/snapshot.h"
#include "walkthrough/session.h"

namespace fv::dictation {

// Where a finished walkthrough goes once the transcription runtime has it (the ui_controller
// methods services/runtime/transcription.py _deliver_walkthrough and _copy_walkthrough call):
// the clipboard, the ready card, the two-step paste to an agent and the live column. The
// walkthrough feature implements it over the clipboard service, the ready card and the
// walkthrough's column. The status line is the runtime's status_update, as for every other job.
// Affinity: UI actor; the runtime calls it on the UI executor only.
class WalkthroughDeliveryPort {
public:
    virtual ~WalkthroughDeliveryPort() = default;

    // Puts `paths` (as files) and `text` on the clipboard; either may be empty. False when the
    // clipboard could not be written.
    virtual bool copy_files_to_clipboard(const std::vector<std::filesystem::path>& paths,
                                         const std::string& text) = 0;
    // The card that says the walkthrough is ready; `copied` is whether the copy above worked.
    virtual void show_walkthrough_ready(const walkthrough::WalkthroughResult& result,
                                        bool copied) = 0;
    // The Agent copy format with auto-paste: the PDF, then the instructions, pasted into
    // `target` (the app that had focus when the card was about to show; nullopt when unknown).
    // Called right after show_walkthrough_ready.
    virtual void paste_walkthrough(const walkthrough::WalkthroughResult& result,
                                   std::optional<focus::AppIdentity> target) = 0;
    // The column said "Writing it up…" and nothing is coming: it goes at once.
    virtual void dismiss_walkthrough_panel() = 0;
};

} // namespace fv::dictation
