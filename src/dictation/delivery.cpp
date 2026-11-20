#include "dictation/delivery.h"

#include <optional>

#include "base/log.h"
#include "settings/keys.h"

namespace fv::dictation {

namespace {
constexpr log::Logger kLog{"services.runtime.transcription"};
} // namespace

bool apply_clipboard_and_paste(const DeliveryPort& port, const Json& settings,
                               const std::string& transcript, const StatusReport& report,
                               std::string_view status_suffix, const std::string& html,
                               bool force_paste) {
    const bool copy_clipboard = keys::copy_clipboard.read(settings);
    const bool auto_paste = force_paste || keys::auto_paste.read(settings);
    const auto status = [&](std::string_view text) {
        report(std::string(text) + std::string(status_suffix));
    };
    // Windows always lets a program send keystrokes; macOS's Accessibility gate does not apply.
    if (auto_paste) {
        return paste_transcript(port, settings, transcript, html, status);
    }
    // Only a paste consumes the clipboard snapshot prefetched when the recording started
    // (auto-paste may have been turned off since).
    port.clipboard->discard_prefetch();
    bool copy_ok = false;
    if (copy_clipboard) {
        copy_ok = copy_transcript(port, transcript, html);
    }
    if (copy_clipboard && !copy_ok) {
        status(COPY_FAILED);
        return false;
    }
    status(READY);
    return false;
}

bool paste_transcript(const DeliveryPort& port, const Json& settings, const std::string& transcript,
                      const std::string& html, const StatusReport& report) {
    std::optional<std::string> rich;
    if (!html.empty()) {
        rich = html;
    }
    const bool restore = keys::restore_clipboard.read(settings);
    const focus::StageResult stage = port.clipboard->stage_text(transcript, rich, restore);
    if (!stage.written) {
        kLog.error("Failed to copy transcription for auto-paste");
        report(std::string(COPY_FAILED));
        return false;
    }
    kLog.info("Transcription copied to clipboard for auto-paste");
    try {
        port.keys->send_paste();
        kLog.info("Transcription auto-pasted");
    } catch (const std::exception& error) {
        kLog.error_with(error, "Failed to auto-paste");
        // ui_controller.commit_transcript_clipboard: without a lease the staged text already
        // stays.
        const bool kept =
            stage.lease ? port.clipboard->commit_text(*stage.lease, transcript) : stage.written;
        if (!kept) {
            kLog.warning("Could not leave transcription in clipboard after paste failure");
        }
        report(std::string(PASTE_FAILED));
        return false;
    }
    if (stage.restore_unavailable) {
        kLog.warning("Transcription pasted, but the previous clipboard could not be captured");
        report(std::string(READY_PASTED_NO_RESTORE));
        return true;
    }
    report(std::string(READY_PASTED));
    if (stage.lease) {
        port.clipboard->schedule_restore(*stage.lease, focus::CLIPBOARD_RESTORE_DELAY);
    }
    return true;
}

bool copy_transcript(const DeliveryPort& port, const std::string& transcript,
                     const std::string& html) {
    const bool copied = port.clipboard->write_text(transcript, html);
    if (copied) {
        kLog.info("Transcription copied to clipboard");
    } else {
        kLog.error("Failed to copy to clipboard");
    }
    return copied;
}

} // namespace fv::dictation
