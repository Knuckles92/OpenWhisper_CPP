#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QString>

#include "clipboard/backend.h"
#include "clipboard/render.h"
#include "clipboard/sequence.h"
#include "clipboard/snapshot.h"
#include "focus/clipboard_port.h"

class QClipboard;
class QTimer;

namespace fv {
class Clock;
}

namespace fv::clipboard {

// The private format that marks a stage as ours; its bytes are the lease's 16 random bytes.
// "application/x-<product in lower case>-auto-paste": only the writer reads its own marker.
extern const QString kAutoPasteMarkerFormat;

// Delay between a recording starting and its clipboard prefetch. The capture holds the GUI
// thread (60 ms for a 4K screenshot copied by a .NET app such as ShareX, up to 150 ms when a Qt
// app offers it in every image format), so it waits until the waveform overlay (30 fps) has
// painted its first frames; the user goes on speaking for seconds after this.
inline constexpr std::chrono::milliseconds kPrefetchDelay = focus::CLIPBOARD_PREFETCH_DELAY;
// How often a selection capture looks for the copy to land.
inline constexpr std::chrono::milliseconds kSelectionPoll{10};
// Without a change counter, every this many polls the clipboard's text is compared with the
// original, for copies Qt does not signal.
inline constexpr int kSelectionTextPolls = 5;

struct TemporaryClipboardProbe;

// Stages transcript text and restores the user's clipboard if the stage is still ours.
//
// Capturing the user's clipboard copies every format, and with a screenshot there 6 of 28 logged
// pastes waited 100-263 ms on it. request_prefetch takes the snapshot while the user is still
// speaking, and stage_text reuses it only while the clipboard provably has not changed.
//
// Affinity: UI actor (lives on the GUI thread). request_prefetch, discard_prefetch and
// capture_selection are safe from any thread: they hop to the GUI thread, never touching the
// clipboard inline in a caller that is busy starting the recorder and overlay.
class TemporaryClipboard final : public QObject, public focus::ClipboardPort {
    Q_OBJECT

public:
    // clipboard may be null (no clipboard: every write fails). sequence validates a prefetched
    // snapshot (system_clipboard_sequence() in the app); without one prefetching is disabled.
    // renderer pre-renders a stage (system_renderer() in the app). sees_foreign_copies defaults to
    // foreign_copies_visible().
    TemporaryClipboard(QClipboard* clipboard, SequenceSource sequence, Renderer renderer,
                       QObject* parent = nullptr,
                       std::optional<bool> sees_foreign_copies = std::nullopt);
    // The same over any backend (tests make its reads and writes fail); backend may be null.
    TemporaryClipboard(std::unique_ptr<ClipboardBackend> backend, SequenceSource sequence,
                       Renderer renderer, QObject* parent = nullptr,
                       std::optional<bool> sees_foreign_copies = std::nullopt);
    ~TemporaryClipboard() override;

    focus::StageResult stage_text(std::string text, std::optional<std::string> html,
                                  bool restore = true) override;
    bool schedule_restore(const focus::ClipboardLease& lease,
                          std::chrono::milliseconds delay) override;
    bool commit_text(const focus::ClipboardLease& lease, std::string text) override;
    focus::RestoreOutcome restore_now(const focus::ClipboardLease& lease) override;
    void cleanup() override;

    void request_prefetch(std::chrono::milliseconds delay = kPrefetchDelay) override;
    void discard_prefetch() override;

    // Order: a pending restore is resolved; the user's clipboard is kept (the prefetch while it
    // is provably current, else a fresh capture); send_copy() runs; the clipboard is watched for
    // the copy to land; its text is read and the original written straight back and kept as the
    // next stage's snapshot; only then callback(text) runs.
    void capture_selection(std::function<void()> send_copy,
                           std::function<void(std::optional<std::string>)> callback,
                           std::chrono::milliseconds timeout) override;

    bool write_text(std::string text, std::string html = {}) override;
    // png_image is decoded into the image writeFiles puts on the clipboard.
    bool write_files(focus::ClipboardFiles files) override;

    // Puts paths on the clipboard as files, with text and image: each app pastes what it takes
    // (a chat app or the file manager the files, which Windows reads as CF_HDROP; a terminal the
    // text; an app that takes pictures the image). Like write_text, it ends a selection capture
    // in flight, and once written drops any pending restore.
    bool writeFiles(const std::vector<std::filesystem::path>& paths, const QString& text = {},
                    const std::optional<QImage>& image = std::nullopt,
                    const QByteArray& marker = {});
    bool markerPresent(std::string_view marker);

Q_SIGNALS:
    // The user's clipboard could not be put back (as restore_failed, for Qt connections).
    void restoreFailed(const QString& message);

private:
    friend struct TemporaryClipboardProbe;

    void startPrefetch(std::chrono::milliseconds delay);
    void capturePrefetch();
    void discardPrefetchNow();
    std::shared_ptr<const ClipboardSnapshot> takePrefetchedSnapshot();
    std::shared_ptr<const ClipboardSnapshot> captureSnapshot();
    std::optional<std::uint64_t> readSequence() const;

    void startSelectionCapture(std::function<void()> send_copy,
                               std::function<void(std::optional<std::string>)> callback,
                               std::chrono::milliseconds timeout);
    void pollSelection();
    void onClipboardDataChanged();
    void abortSelectionCapture();
    void finishSelection(const QString& text);
    QString clipboardText();
    static void callBack(const std::function<void(std::optional<std::string>)>& callback,
                         std::optional<std::string> text);

    void restorePending();
    void resolvePendingBeforeStage();
    focus::RestoreOutcome restore(const focus::ClipboardLease& lease);
    bool owns(const focus::ClipboardLease& lease);
    bool writeText(const QString& text, const QString& html);
    bool stageWithoutLease(const QString& text, const QString& html);
    void renderStagedText(bool rich);
    void discardPending();
    // Our own write replaced the clipboard, so no earlier lease's marker is there any more. Leases
    // whose restore failed are kept (the newest kKeptFailedLeases), as StagedLease says.
    void forgetLeases();
    static constexpr std::size_t kKeptFailedLeases = 8;
    void emitRestoreFailed(const std::string& message);

    std::unique_ptr<ClipboardBackend> backend_;
    bool sees_foreign_copies_;
    SequenceSource sequence_;
    Renderer renderer_;
    const Clock* clock_;

    std::optional<focus::ClipboardLease> pending_;
    // What each lease that may still be on the clipboard needs for a restore, by lease id.
    std::map<std::uint64_t, StagedLease> leases_;
    std::uint64_t next_lease_id_ = 0;
    QTimer* restore_timer_;

    std::optional<PrefetchedSnapshot> prefetched_;
    QTimer* prefetch_timer_;

    std::unique_ptr<SelectionCapture> selection_;
    QTimer* selection_timer_;

    // Clipboard captures taken (ClipboardSnapshot::capture calls), for tests.
    int captures_ = 0;
};

} // namespace fv::clipboard
