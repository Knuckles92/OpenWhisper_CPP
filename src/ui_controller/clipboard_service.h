#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <QObject>
#include <QString>

#include "focus/clipboard_port.h"

namespace fv {

namespace clipboard {
class TemporaryClipboard;
}
namespace focus {
class SyntheticKeys;
}

// The status the line shows when the user's clipboard could not be put back after a paste.
inline constexpr char kRestoreFailedStatus[] = "Ready (Pasted; clipboard restore failed)";

// The UI controller's clipboard (ui_controller.py:1001-1065): the TemporaryClipboard every
// delivery stages through, the copies the window and the tray ask for, the selection a Command
// Mode recording reads (M4 calls it), and the status a failed restore leaves. The transcription
// runtime reaches the same TemporaryClipboard as its focus::ClipboardPort.
// Affinity: GUI thread; prefetch, discardPrefetch and captureSelection from any thread.
class ClipboardService : public QObject {
    Q_OBJECT
public:
    ClipboardService(std::shared_ptr<clipboard::TemporaryClipboard> clipboard,
                     std::shared_ptr<focus::SyntheticKeys> keys, QObject* parent = nullptr);
    ~ClipboardService() override;

    clipboard::TemporaryClipboard& clipboard() const { return *clipboard_; }

    // copy_to_clipboard: text, and rich text when given; true when the write succeeded.
    bool copyToClipboard(const QString& text, const QString& html = {});
    // prefetch_clipboard_snapshot and discard_clipboard_prefetch.
    void prefetch();
    void discardPrefetch();
    // stage_transcript_for_paste: an empty html is none; restore off keeps no lease.
    focus::StageResult stageTranscriptForPaste(const std::string& text,
                                               const std::optional<std::string>& html = {},
                                               bool restore = true);
    // capture_selection: the copy shortcut is the synthetic keys' send_copy, and the timeout
    // config.COMMAND_SELECTION_TIMEOUT_MS unless given.
    void captureSelection(std::function<void(std::optional<std::string>)> callback,
                          std::optional<std::chrono::milliseconds> timeout = std::nullopt);
    // schedule_clipboard_restore: after AUTO_PASTE_CLIPBOARD_RESTORE_DELAY_MS; false without a
    // lease.
    bool scheduleClipboardRestore(const focus::StageResult& stage);
    // commit_transcript_clipboard: without a lease the stage stands as written.
    bool commitTranscriptClipboard(const focus::StageResult& stage, const std::string& text);
    // Puts the user's clipboard back now if a stage still holds it (UIController.cleanup).
    void cleanup();

Q_SIGNALS:
    // _on_clipboard_restore_failed's status.
    void statusRequested(const QString& status);

private:
    std::shared_ptr<clipboard::TemporaryClipboard> clipboard_;
    std::shared_ptr<focus::SyntheticKeys> keys_;
};

} // namespace fv
