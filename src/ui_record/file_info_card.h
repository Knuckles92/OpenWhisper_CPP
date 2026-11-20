#pragma once

#include <optional>

#include <QFrame>
#include <QString>
#include <QStringList>

#include "audio/file_preview.h"

class QLabel;
class QPushButton;
class QStackedWidget;
class QTimer;

namespace fv {

class Button;
class DecodeLabel;
class ElidingLabel;
class PrimaryButton;
class TranscriptionProgressPanel;
class UploadQueueWidget;

// What a computer that transcribed for this one adds to the result note: its name, and the time
// spent on the network when it answered at least one request.
struct RemoteResultTiming {
    QString host;
    std::optional<double> networkSeconds;
};

// The loaded file, or the queue of them, and a footer that is the action row or, while a job
// runs, the inline progress panel. The footer is a QStackedWidget so the card keeps one height
// whichever page is showing; swapping visible widgets instead would jog everything below it twice
// per job. One file shows the classic header; two or more swap it for the queue, so the
// single-file path runs the same widgets it always did. Affinity: GUI thread.
class FileInfoCard : public QFrame {
    Q_OBJECT
public:
    // RESULT_HOLD_MS: how long the finished progress panel stays up before the action row
    // returns.
    static constexpr int kResultHoldMs = 1400;

    enum class Mode { Single, Queue };

    explicit FileInfoCard(QWidget* parent = nullptr);

    bool isTranscribing() const { return transcribing_; }
    // True while the footer shows the progress panel rather than the actions.
    bool isProgressShown() const;
    Mode mode() const { return mode_; }

    // Shows the single-file header or the multi-file queue; Copy reads "Copy all" for a queue.
    void setMode(Mode mode);
    // Fills the chips: size, duration, rate ("44.1 kHz"), "Mono" / "Stereo" / "N ch", and the
    // chunk chip ("One pass" in the ok tone, "About N chunks" in the warn tone).
    void setPreview(const audio::AudioFilePreview& preview);
    // Names a file whose preview is still being read off the UI thread.
    void setLoading(const QString& fileName);
    // Whether every queued file has been read and can be transcribed. It is separate from the
    // job's own state so readiness does not latch.
    void setReady(bool ready);
    // Locks the card for a job, or returns it to idle at once (the file was removed).
    // withCleanup: whether the AI cleanup pass is on, so the stepper knows whether to show a
    // third step. totalFiles: files in the job; more than one makes the progress bar
    // determinate.
    void setTranscribing(bool active, bool withCleanup = false, int totalFiles = 1);
    // Ends the job. The result stays up for kResultHoldMs before the actions return. A panel
    // already at Canceled keeps saying so: the runtime follows a cancel with an error
    // transcript, which must not relabel it Failed.
    void finishTranscribing(bool success);
    // Copy is on only while a non-empty transcript is available.
    void setCopyEnabled(bool enabled);
    // How long the job took and how that compares to the audio: "Transcribed in 1m 2s ·
    // 13.6× realtime · Cleaned in 3.2s", through the decode reveal. cleanupTime counts when
    // positive. animate: false shows the finished text at once (the reveal is a timer).
    void setResult(double transcriptionTime, double audioDuration,
                   std::optional<double> cleanupTime = std::nullopt,
                   const std::optional<RemoteResultTiming>& remote = std::nullopt,
                   bool animate = true);
    // Says why the job failed where its timing would have gone: danger tone, the message as its
    // tooltip, no reveal.
    void setErrorNote(const QString& message);
    // The spec's contract name (M2.md, M2.14) for setErrorNote; Python's card has only
    // set_error_note, so the two are one function.
    void setError(const QString& message) { setErrorNote(message); }
    void clearResult();

    TranscriptionProgressPanel* progressPanel() const { return progress_; }
    UploadQueueWidget* queue() const { return queue_; }
    // Elapsed until the actions return after a finished job; active while the result is held.
    QTimer* settleTimer() const { return settleTimer_; }

    QWidget* headerRow() const { return headerRow_; }
    QLabel* iconLabel() const { return iconLabel_; }
    ElidingLabel* filenameLabel() const { return filenameLabel_; }
    QLabel* sizeChip() const { return sizeChip_; }
    QLabel* durationChip() const { return durationChip_; }
    QLabel* rateChip() const { return rateChip_; }
    QLabel* channelsChip() const { return channelsChip_; }
    QLabel* chunkLabel() const { return chunkLabel_; }
    QPushButton* removeButton() const { return removeButton_; }
    Button* copyButton() const { return copyButton_; }
    DecodeLabel* resultLabel() const { return resultLabel_; }
    PrimaryButton* transcribeButton() const { return transcribeButton_; }
    QStackedWidget* footer() const { return footer_; }

Q_SIGNALS:
    void transcribeClicked();
    void removeClicked();
    void copyClicked();
    // Stop in the progress panel.
    void stopClicked();
    // Audio paths dropped onto the card itself, and how many dropped items were not audio.
    void filesDropped(const QStringList& paths, int skipped);
    // True while the footer shows the progress panel rather than the actions.
    void progressShown(bool shown);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void applyTranscribeEnabled();
    void showProgress();
    void showActions();

    bool transcribing_ = false;
    bool ready_ = true;
    Mode mode_ = Mode::Single;
    QTimer* settleTimer_;

    QWidget* headerRow_;
    QLabel* iconLabel_;
    ElidingLabel* filenameLabel_;
    QLabel* sizeChip_;
    QLabel* durationChip_;
    QLabel* rateChip_;
    QLabel* channelsChip_;
    QLabel* chunkLabel_;
    QPushButton* removeButton_;
    UploadQueueWidget* queue_;
    QStackedWidget* footer_;
    QWidget* actionsPage_;
    Button* copyButton_;
    DecodeLabel* resultLabel_;
    PrimaryButton* transcribeButton_;
    TranscriptionProgressPanel* progress_;
};

} // namespace fv
