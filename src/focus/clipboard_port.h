#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "base/signal.h"

// The clipboard as dictation delivery uses it: stage the transcript for a paste and put the
// user's clipboard back afterwards, prefetch while recording, read the selection through a copy,
// and plain writes. Qt-free, so the dictation core codes against it; fv_clipboard's
// TemporaryClipboard implements it on the GUI thread (Python's ui_qt/clipboard.py).
namespace fv::focus {

// config.AUTO_PASTE_CLIPBOARD_RESTORE_DELAY_MS, CLIPBOARD_PREFETCH_DELAY_MS and
// COMMAND_SELECTION_TIMEOUT_MS.
inline constexpr std::chrono::milliseconds CLIPBOARD_RESTORE_DELAY{250};
inline constexpr std::chrono::milliseconds CLIPBOARD_PREFETCH_DELAY{100};
inline constexpr std::chrono::milliseconds SELECTION_TIMEOUT{700};

// One staged paste. The implementation keeps the user's clipboard it stands for; the lease only
// names it. Two leases are the same lease when their ids are.
// Affinity: value type.
struct ClipboardLease {
    std::uint64_t id = 0;
    // Rich text staged with the transcript, kept when the paste fails.
    std::string html;

    bool operator==(const ClipboardLease& other) const { return id == other.id; }
};

// Affinity: value type.
struct StageResult {
    bool written = false;
    // Present when the user's clipboard was kept to restore.
    std::optional<ClipboardLease> lease;
    // The text was staged, but the user's clipboard could not be kept.
    bool restore_unavailable = false;
};

enum class RestoreOutcome { Restored, Skipped, Failed };

// What write_files puts on the clipboard: files (CF_HDROP on Windows) for apps that take them,
// the text for those that take text, an image (encoded PNG bytes) for those that take pictures,
// and an optional private marker.
struct ClipboardFiles {
    std::vector<std::filesystem::path> paths;
    std::string text;
    std::vector<std::uint8_t> png_image;
    std::string marker;
};

// Affinity: UI actor (GUI thread), except request_prefetch, discard_prefetch and
// capture_selection, which are safe from any thread and do their work on the GUI thread.
class ClipboardPort {
public:
    virtual ~ClipboardPort() = default;

    // Puts text on the clipboard for a paste, keeping the user's to restore. html is a rich-text
    // alternative staged with text: apps that take rich text paste it, the rest paste text. With
    // restore off the user's clipboard is not copied, so no lease is returned and the text
    // stays after the paste.
    virtual StageResult stage_text(std::string text, std::optional<std::string> html,
                                   bool restore = true) = 0;
    // Puts the user's clipboard back after delay, if the staged text is still ours by then.
    // False (and the lease dropped) when the clipboard changed first.
    virtual bool schedule_restore(const ClipboardLease& lease, std::chrono::milliseconds delay) = 0;
    // Leaves text, with any rich text staged alongside, on the clipboard instead of restoring.
    virtual bool commit_text(const ClipboardLease& lease, std::string text) = 0;
    virtual RestoreOutcome restore_now(const ClipboardLease& lease) = 0;
    // Ends any selection capture and prefetch, and restores a pending lease now.
    virtual void cleanup() = 0;

    // Snapshots the clipboard delay from now for the next stage. Replaces an earlier prefetch,
    // and does nothing where clipboard changes cannot be detected.
    virtual void request_prefetch(std::chrono::milliseconds delay = CLIPBOARD_PREFETCH_DELAY) = 0;
    // Drops the prefetched snapshot, or cancels one not yet taken; applied after any prefetch
    // requested before it.
    virtual void discard_prefetch() = 0;

    // Reads the focused app's selection through a copy, then puts the user's clipboard back.
    // send_copy sends the copy shortcut. callback receives the selected text, or "" when the
    // clipboard did not change within timeout, nothing could be read, or the copy could not be
    // sent; nullopt when this clipboard can't see other apps' copies (nothing is copied or
    // written then). The work and the callback run on the GUI thread.
    virtual void capture_selection(std::function<void()> send_copy,
                                   std::function<void(std::optional<std::string>)> callback,
                                   std::chrono::milliseconds timeout) = 0;

    // Plain writes: they end a selection capture in flight and, once written, drop any pending
    // restore.
    virtual bool write_text(std::string text, std::string html = {}) = 0;
    virtual bool write_files(ClipboardFiles files) = 0;

    // The user's clipboard could not be put back; the message is for the status line.
    Signal<std::string> restore_failed;
};

} // namespace fv::focus
