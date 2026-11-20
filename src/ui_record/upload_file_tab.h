#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include "appcore/overlay_state.h"
#include "audio/file_preview.h"
#include "dictation/batch.h"
#include "ui_record/transcription_tab_base.h"
#include "ui_record/upload_queue.h"

class QPushButton;

namespace fv {

class BatchRelationDialog;
class DropZoneWidget;
class FileInfoCard;
class TranscriptViewerDialog;

// What a preview worker found for one file: its facts, or the sentence that says why it could not
// be read. Affinity: value type.
struct UploadPreviewResult {
    std::uint64_t token = 0;
    QString path;
    std::optional<audio::AudioFilePreview> preview;
    QString error;
};

// _PreviewRelay: brings file previews to the GUI thread for whichever upload tabs exist then. It
// lives for the process, so a preview worker never holds or posts to a tab: holding one let a
// worker delete the tab off the GUI thread, and posting to one raced its deletion. A deleted tab
// drops out of the relay's connections. Affinity: GUI thread; post() from any thread.
class UploadPreviewRelay : public QObject {
    Q_OBJECT
public:
    // The relay of the process, made on first use; call that first use on the GUI thread.
    static UploadPreviewRelay& instance();
    // Delivers result on the relay's thread (through ready).
    void post(UploadPreviewResult result);
    // Tokens unique across tabs, because they share the relay.
    static std::uint64_t nextToken();

Q_SIGNALS:
    void ready(const UploadPreviewResult& result);

private:
    UploadPreviewRelay() = default;
};

// Tab widget for uploading and transcribing audio files. One file takes the single-file path
// (uploadRequested); two or more become a queue and go out as one BatchUploadRequest. File facts
// are read off the GUI thread, one worker per drop, and come back by path through the
// relay. The tab runs nothing itself: intent leaves as signals and the presenter answers through
// the setters. Affinity: GUI thread.
class UploadFileTab : public TranscriptionTabBase {
    Q_OBJECT
public:
    // Reads one file's facts for an engine that does (engineSplits) or does not split a file
    // over the upload limit. Runs on a preview worker; throws as audio::preview_file does.
    using PreviewReader = std::function<audio::AudioFilePreview(const std::filesystem::path& path,
                                                                bool engineSplits)>;
    // Starts a preview worker (_run_in_thread). The default runs it on a detached thread.
    using PreviewRunner = std::function<void(std::function<void()> worker)>;
    // Asks for audio files (the Select Audio Files dialog); empty when cancelled.
    using FileBrowser = std::function<QStringList(QWidget* parent)>;

    UploadFileTab(SettingsStore& settings, LocalEngineCatalog catalog, QWidget* parent = nullptr);
    ~UploadFileTab() override;

    // audio::preview_file with speech::time_based_splits as its chunk estimator.
    static audio::AudioFilePreview readPreview(const std::filesystem::path& path,
                                               bool engineSplits);

    // Seams for tests; the defaults read real files on a detached thread and open the native
    // dialog.
    void setPreviewReader(PreviewReader reader) { reader_ = std::move(reader); }
    void setPreviewRunner(PreviewRunner runner) { runner_ = std::move(runner); }
    void setFileBrowser(FileBrowser browser) { browser_ = std::move(browser); }
    // os.path.exists, at Transcribe time; tests replace it.
    void setFileExists(std::function<bool(const QString&)> exists) { exists_ = std::move(exists); }
    // Runs the Custom description dialog and says whether it was accepted. Unset, it runs
    // modally; a test sets one to answer it without a nested event loop.
    void setRelationDialogRunner(std::function<bool(BatchRelationDialog&)> runner) {
        relationDialogRunner_ = std::move(runner);
    }

    // ---- state ----

    // Whether a job started from this tab is still running.
    bool isTranscribing() const;
    QStringList queuedPaths() const;
    const QList<QueueItem>& items() const { return items_; }
    // The relation preset ("separate", "sequential", "custom").
    dictation::BatchRelation relation() const { return relation_; }
    const QString& customInstructions() const { return customInstructions_; }
    bool customCombine() const { return customCombine_; }
    // The single queued file (the single-file callers' mirror); empty for none or a queue.
    const QString& audioPath() const { return audioPath_; }

    // ---- job ----

    // Takes a stage the overlay would have shown for hotkey dictation. None while the job is
    // still marked running means it ended without a transcript reaching this tab: the card is
    // released as a failure.
    void setProgressState(appcore::OverlayState state);
    // "Splitting large file", "{x.x} MB file"; only while a job runs.
    void setLargeFileStage(double fileSizeMb);
    // A file of the running batch is starting (1-based position).
    void setBatchProgress(int position, int total, const QString& sourceName);
    // A file of the running batch has ended, with its own text if any.
    void setBatchItemFinished(int position, bool success, const QString& transcript = {});
    // The per-file structure behind a batch, for the reading window. It is kept until the next
    // setTranscript or clearTranscription, as it may arrive before or after the text.
    void setBatchResult(const dictation::BatchResult& result);
    // Ends a failed or canceled job: the card, not the transcript, says why (nothing for a
    // cancel).
    void showJobError(const QString& message);

    // Finishes the job: a text starting "Error:" failed it (a cancel when it says "canceled"),
    // anything else succeeded; Copy and the reading window's button follow whether there is a
    // transcript to copy.
    void setTranscript(const QString& text,
                       const std::optional<QString>& raw = std::nullopt) override;
    void clearTranscription() override;
    // The result goes into the card ("Transcribed in ..."); this tab never shows the stats strip.
    void setTranscriptionStats(double transcriptionTime, double audioDuration,
                               std::optional<std::int64_t> fileSize, double cleanupTime = 0.0,
                               const std::optional<remote::RemoteTiming>& remote = {}) override;
    void setBackend(const QString& displayName) override;

    // ---- queue ----

    // The paths a drop or the browser gave, and how many dropped items were not audio. Ignored
    // while a job runs.
    void addFiles(const QStringList& paths, int skipped = 0);
    // Resets the queue, hides the card and shows the drop zone.
    void clearFile();
    // Moves a queued file up (-1) or down (+1).
    void moveFile(const QString& path, int delta);
    void removeFile(const QString& path);
    // The Transcribe button.
    void transcribe();
    // The Stop button: the tab stays locked until the job's terminal signal arrives.
    void stop();
    // Opens Select Audio Files and queues what was chosen.
    void openFileBrowser();
    // Shows the current transcript in the reading window, creating it once.
    void openTranscriptViewer();
    // The reading window once opened, else null.
    TranscriptViewerDialog* viewer() const;

    // A preview arrived (_on_preview_ready). A result for another tab's token or for a path no
    // longer queued is ignored.
    void onPreviewReady(const UploadPreviewResult& result);
    std::uint64_t previewToken() const { return previewToken_; }

    DropZoneWidget* dropZone() const { return dropZone_; }
    FileInfoCard* fileInfoCard() const { return fileInfoCard_; }
    QPushButton* expandButton() const { return expandButton_; }

public Q_SLOTS:
    // A runtime message shows in the progress panel while a job is up; outside a job it is
    // dropped, because the card and the drop zone already say what state the tab is in.
    void setStatus(const QString& statusText) override;
    // Lets files be browsed and queued while the recorder is in use, but not transcribed. The
    // main window's recording relay calls it by name.
    void setRecordingActive(bool active);
    void clearTranscriptionStats() override;

Q_SIGNALS:
    // One file: its path and the preview's duration in seconds (0 without one).
    void uploadRequested(const QString& path, double durationSeconds);
    void uploadFilesRequested(const dictation::BatchUploadRequest& request);
    void cancelRequested();
    // Text for the clipboard: the card's Copy (the Fixed text), a row's own text, or the reading
    // window's shown version.
    void copyRequested(const QString& text);
    // The stats strip was shown or hidden (it never is on this tab: the result goes to the card).
    void statsVisibilityChanged(bool visible);

protected:
    void buildContentBeforeStatus(QVBoxLayout* layout) override;
    void connectSignals() override;
    void onBackendChanged() override;

private:
    QueueItem* itemFor(const QString& path);
    void render();
    void refreshReady();
    void startPreviews(const QStringList& paths);
    bool engineSplitsFiles() const;
    void refreshChunkEstimates();
    void applyRelationToPicker();
    void onRelationChanged(const QString& value);
    void onEditCustom();
    bool editCustomDescription();
    void releaseWorkingQueueRows(bool success, bool canceled = false);
    void syncViewer();
    QString viewerTitle() const;
    void updateEngineLock();
    void copyFixed();
    void copyRow(const QString& path);
    void resetResult();
    void pushRows();

    PreviewReader reader_;
    PreviewRunner runner_;
    FileBrowser browser_;
    std::function<bool(const QString&)> exists_;
    std::function<bool(BatchRelationDialog&)> relationDialogRunner_;

    QList<QueueItem> items_;
    bool recordingActive_ = false;
    QString audioPath_;
    std::optional<audio::AudioFilePreview> preview_;
    // What the last drop left out (non-audio, duplicates) or what the last Transcribe removed;
    // shown in the queue header until the next drop, clear or job.
    QString queueNote_;
    // Stop on this tab, until the job's terminal signal settles the rows.
    bool stopRequested_ = false;
    dictation::BatchRelation relation_ = dictation::BatchRelation::Separate;
    QString customInstructions_;
    bool customCombine_ = false;
    std::optional<dictation::BatchResult> batchResult_;
    std::uint64_t previewToken_ = 0;
    // The text set_transcript last showed, for the card's Copy and the reading window.
    QString fixedText_;
    std::optional<QString> rawText_;

    DropZoneWidget* dropZone_ = nullptr;
    FileInfoCard* fileInfoCard_ = nullptr;
    QPushButton* expandButton_ = nullptr;
    QPointer<TranscriptViewerDialog> viewer_;
};

} // namespace fv

Q_DECLARE_METATYPE(fv::UploadPreviewResult)
