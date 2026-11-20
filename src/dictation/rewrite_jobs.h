#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "dictation/cleanup_info.h"
#include "dictation/job.h"
#include "dictation/pipeline.h"

// Rewrites of selected text on the transcription job slot (services/runtime/transcription.py
// 2498-2519, 2778-2875): what a Command Mode recording or a transform hands the runtime, and the
// texts a rewrite shows when it is copied instead of pasted or once it is pasted.
namespace fv::dictation {

// (text, raw_text, info) as a dictation's cleanup returns it: the text to paste, the selection it
// replaces (nullopt when blank), and the model that wrote it.
// Affinity: value type.
struct RewriteResult {
    std::string text;
    std::optional<std::string> raw_text;
    std::optional<CleanupInfo> info;

    bool operator==(const RewriteResult&) const = default;
};

// The work submit_rewrite runs on the jobs strand. It throws CommandRefused (command_selection.h)
// for a refusal the user acts on, anything else for a failure whose message is shown.
using RewriteWork = std::function<RewriteResult()>;

// What turns a Command Mode or transform recording's transcript into the text to paste
// (CommandRuntime). complete_recording runs on the jobs strand while the job holds the slot and
// throws as RewriteWork does.
class CommandCompletion {
public:
    virtual ~CommandCompletion() = default;
    virtual RewriteResult complete_recording(const std::string& raw,
                                             const std::shared_ptr<const DictationJob>& job) = 0;
    // Rewrites `text` with a saved transform's instruction, as an action shaped by a transform
    // does with its own dictated words; throws as complete_recording does. Jobs strand, slot
    // held. Without a Command Mode runtime there is nothing to rewrite with.
    virtual RewriteResult rewrite_text(const std::string& text, const std::string& instruction) {
        (void)text;
        (void)instruction;
        throw std::runtime_error("Command Mode isn't available");
    }
};

// _copy_rewrite's status: "Rewrite copied — the app changed; paste it where you want" (or
// "couldn't confirm the app" for any other target) when the copy was written, else the copy
// failure. It is shown on the status line and near the pointer.
std::string rewrite_copied_status(bool copied, PasteTarget target);

// A notice near the pointer: its text and whether it confirms something done.
// Affinity: value type.
struct PointerNotice {
    std::string text;
    bool done = false;

    bool operator==(const PointerNotice&) const = default;
};

// _notify_forced_outcome: a pasted rewrite says "Rewritten" (or "Inserted" when nothing was
// selected) as done; a failed paste repeats `delivery_status`. nullopt when there is nothing to
// show (an empty status).
std::optional<PointerNotice> forced_outcome_notice(bool pasted, bool had_selection,
                                                   std::string_view delivery_status);

} // namespace fv::dictation
