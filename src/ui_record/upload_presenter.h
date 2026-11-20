#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

#include <QObject>
#include <QPointer>
#include <QString>

#include "appcore/overlay_state.h"
#include "dictation/batch.h"

namespace fv {

class ScreenRegistry;
class UploadFileTab;

namespace dictation {
class TranscriptionRuntime;
struct TranscriptionStats;
} // namespace dictation
namespace focus {
class ClipboardPort;
}

// The Upload File tab's side of the UI controller (ui_controller.py 934-970, 2450-2507 and
// main_window.py 749-758): which tab a job's results belong to, the tab's requests to the
// transcription runtime, and the runtime's upload news routed onto the tab's card instead of the
// overlay. RecordFeature::install builds it and provides it (ctx.service<UploadPresenter>());
// the controller calls the routing functions from its own handlers, each of which says whether
// the upload tab took the call. The tab is the "upload" screen, built on first visit; the
// presenter connects its requests when it is built. Affinity: GUI thread.
class UploadPresenter : public QObject {
    Q_OBJECT
public:
    // What the presenter reaches. Every member may be empty: a request with nothing to take it
    // is logged and dropped, a copy with no clipboard fails, and a status with no window goes
    // nowhere.
    struct Ports {
        // The transcription runtime's upload_audio_file, upload_audio_files and cancel.
        std::function<void(const std::filesystem::path& path, std::optional<double> seconds)>
            uploadFile;
        std::function<void(const dictation::BatchUploadRequest& request)> uploadFiles;
        std::function<void()> cancel;
        // The clipboard the UI controller's copies write to (M1.16's TemporaryClipboard).
        focus::ClipboardPort* clipboard = nullptr;
        // The main window's status line (MainWindow::setStatus).
        std::function<void(const QString&)> windowStatus;
        // Quick Record's engine_loading.
        std::function<bool()> quickRecordLoading;
        // The widget the tab is built under when a result of its job arrives before it exists
        // (main_window.py's upload_file_tab builds the screen on that access): the main window's
        // tab stack. Unset, such a result goes to Quick Record.
        std::function<QWidget*()> tabParent;
    };

    UploadPresenter(ScreenRegistry& screens, Ports ports, QObject* parent = nullptr);
    ~UploadPresenter() override;

    // The Upload File tab when it has been built, else null.
    UploadFileTab* tab() const;
    // Connects the tab's requests (uploadRequested, uploadFilesRequested, cancelRequested,
    // copyRequested); a tab already attached is left alone. The "upload" screen is attached when
    // it is created.
    void attach(UploadFileTab* tab);

    // _transcription_source_tab: the last job came from the Upload File tab.
    bool isUploadSource() const { return uploadSource_; }
    // A recording or dictation started from anywhere else: its results go to Quick Record
    // (start_recording and on_dictation_started reset the source).
    void recordingStarted() { uploadSource_ = false; }
    // _upload_job_active: the Upload File tab started the job, exists, and is still running it.
    bool isUploadJobActive() const;

    // _route_upload_progress, while isUploadJobActive(): the state goes to the tab's card. True
    // when it was taken inline, so the caller stops the tray's recording look, hides the overlay
    // and shows nothing there; None goes to the card too but returns false, so the shared
    // teardown still runs. False for a state with no stage of the card.
    bool routeProgress(appcore::OverlayState state);
    // show_large_file_state: true when an upload job took the size inline ("Splitting large
    // file"); false leaves it to the overlay.
    bool showLargeFileState(double fileSizeMb);
    // set_batch_progress and set_batch_item_finished: only while an upload job runs.
    void setBatchProgress(int position, int total, const QString& sourceName);
    void setBatchItemFinished(int position, bool success, const QString& transcript);
    // set_batch_result: the reading window's per-file structure, when the job was the upload
    // tab's.
    void setBatchResult(const dictation::BatchResult& result);

    // _display_transcript and _display_transcription_error for an upload job: true when the
    // upload tab took the result (and expanded its transcript), false when Quick Record should.
    // These, the stats, the batch result and an upload job's status build the tab when it is
    // gone, as OpenWhisper's upload_file_tab property does; the engine-loading status is kept for
    // it instead (the same text shows when it is built).
    bool displayTranscript(const QString& text, const std::optional<QString>& raw);
    bool displayTranscriptionError(const QString& message);
    // set_transcription_stats for an upload job: true when the card took them.
    bool setTranscriptionStats(const dictation::TranscriptionStats& stats);
    // _apply_status_to_main_window: while Quick Record's engine loads, the window's status line
    // and the upload tab's engine readout; for an upload job, the tab's progress panel;
    // otherwise the window's status line.
    void applyStatus(const QString& status);

    // The tab's requests (main_window.py's relay and the UI controller's handlers).
    void requestUpload(const QString& path, double durationSeconds);
    void requestUploads(const dictation::BatchUploadRequest& request);
    // _on_upload_cancel: only while an upload job runs; the recording state is not touched.
    void cancelUpload();
    // _on_upload_copy: "Copied to clipboard" and the copied animation, or "Copy failed".
    void copy(const QString& text);

    // Over the transcription runtime's upload calls.
    static Ports runtimePorts(dictation::TranscriptionRuntime& runtime);

Q_SIGNALS:
    // The overlay's Copied animation (the router shows it).
    void copiedAnimationRequested();

private:
    // tab(), or the tab built now under Ports::tabParent: the upload_file_tab property.
    UploadFileTab* ensureTab();

    ScreenRegistry& screens_;
    Ports ports_;
    bool uploadSource_ = false;
    QPointer<UploadFileTab> attached_;
};

} // namespace fv
