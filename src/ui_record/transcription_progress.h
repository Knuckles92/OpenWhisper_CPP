#pragma once

#include <functional>
#include <optional>

#include <QFrame>
#include <QList>
#include <QString>
#include <QWidget>

#include "appcore/overlay_state.h"
#include "dictation/upload_progress.h"

class QLabel;
class QPushButton;
class QTimer;

namespace fv {

class AnimatedProgressBar;
class ElidingLabel;

// One step of the stepper: a dot and a title, both coloured by state (their "stepState"
// property). Affinity: GUI thread.
class UploadStepChip : public QWidget {
    Q_OBJECT
public:
    explicit UploadStepChip(const QString& title, QWidget* parent = nullptr);

    dictation::StepState state() const { return state_; }
    void setState(dictation::StepState state);

    QLabel* dot() const { return dot_; }
    QLabel* titleLabel() const { return title_; }

private:
    QLabel* dot_;
    QLabel* title_;
    dictation::StepState state_ = dictation::StepState::Pending;
    bool stateSet_ = false;
};

// Inline progress for a transcription the user started inside the window: stage title, a bar,
// and a three-step stepper for one job. The bar sweeps while work is in flight because no backend
// reports a fraction; it fills on completion so the end reads as an end. A batch of files does
// have a fraction (files finished over files total), so the bar turns determinate for one. The
// panel keeps its own clock because the stats strip only appears afterwards, and a CPU
// transcription of a long file can run for minutes with nothing else on screen changing. The
// batch position has its own label rather than sharing the detail label, which the runtime's
// status messages overwrite continuously. Affinity: GUI thread.
class TranscriptionProgressPanel : public QFrame {
    Q_OBJECT
public:
    // _ELAPSED_TICK_MS.
    static constexpr int kElapsedTickMs = 1000;

    explicit TranscriptionProgressPanel(QWidget* parent = nullptr);

    // The stage last set; nullopt before the first start.
    std::optional<dictation::ProgressStage> stage() const { return stage_; }
    // True from start() until a terminal stage is reached.
    bool isRunning() const;
    bool isBatch() const { return batchTotal_ > 1; }

    // Begins a job at the Preparing stage. withCleanup: whether the AI cleanup pass is on, which
    // decides whether the stepper shows its third step. totalFiles: more than one turns the bar
    // determinate and shows the position label.
    void start(bool withCleanup, int totalFiles = 1);
    // Announces which file of the batch is starting (1-based); the position is clamped into
    // 1..total, the label shows what was given.
    void setBatchPosition(int position, int total, const QString& filename);
    void setStage(dictation::ProgressStage stage, const std::optional<QString>& detail = {});
    void setDetail(const QString& text);
    // Acknowledges a Stop press while the runtime winds the job down: Stop off, "Canceling…".
    void setStopping();
    void setStopEnabled(bool enabled);
    // "Splitting large file" with "{x.x} MB file" as its detail.
    void setLargeFile(double fileSizeMb);
    // Maps a routed overlay state onto the stepper; false when the state is no stage of this
    // panel.
    bool applyOverlayState(appcore::OverlayState state);
    // Done or Failed.
    void finish(bool success);

    // The seconds the elapsed clock reads (monotonic); tests replace it.
    void setClock(std::function<double()> now);
    // Writes the elapsed time now: the clock's timeout.
    void tick();

    QLabel* titleLabel() const { return titleLabel_; }
    ElidingLabel* batchLabel() const { return batchLabel_; }
    QLabel* elapsedLabel() const { return elapsedLabel_; }
    QPushButton* stopButton() const { return stopButton_; }
    AnimatedProgressBar* bar() const { return bar_; }
    ElidingLabel* detailLabel() const { return detailLabel_; }
    const QList<UploadStepChip*>& steps() const { return steps_; }
    QWidget* connector(int index) const { return connectors_.value(index); }

Q_SIGNALS:
    void stopClicked();

protected:
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void applySteps(dictation::ProgressStage stage);

    std::optional<dictation::ProgressStage> stage_;
    std::optional<double> startedAt_;
    int batchTotal_ = 1;
    int batchPosition_ = 1;
    std::function<double()> now_;

    QLabel* titleLabel_;
    ElidingLabel* batchLabel_;
    QLabel* elapsedLabel_;
    QPushButton* stopButton_;
    AnimatedProgressBar* bar_;
    ElidingLabel* detailLabel_;
    QList<UploadStepChip*> steps_;
    QList<QWidget*> connectors_;
    QTimer* clock_;
};

} // namespace fv
