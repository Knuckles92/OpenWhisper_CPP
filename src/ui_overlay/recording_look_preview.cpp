#include "ui_overlay/recording_look_preview.h"

#include <chrono>
#include <exception>
#include <string>
#include <system_error>

#include <QHBoxLayout>
#include <QHideEvent>
#include <QList>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/clock.h"
#include "base/identity.h"
#include "base/log.h"
#include "pyport/text.h"
#include "recorder/recorder.h"
#include "ui_overlay/waveform_overlay.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"
#include "uimath/sample_voice.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.recording_look_preview"};

} // namespace

RecordingLookPreview::RecordingLookPreview(
    std::function<std::unique_ptr<Recorder>()> recorderFactory,
    std::function<QString()> microphoneName, QWidget* parent, const SettingsStore* settings,
    const Clock* clock)
    : QWidget(parent), recorderFactory_(std::move(recorderFactory)),
      microphoneName_(std::move(microphoneName)),
      clock_(clock != nullptr ? clock : &system_clock()),
      ui_(std::make_unique<qt::QtUiExecutor>(this)), stage_(kStages.front().name) {
    setObjectName(QStringLiteral("recordingLookPreview"));

    overlay_ = new WaveformOverlay(this, WaveformOverlay::Mode::Preview, settings, clock_);
    overlay_->hide();
    auto* stage = new QHBoxLayout();
    stage->setContentsMargins(0, 6, 0, 6);
    stage->addStretch(1);
    stage->addWidget(overlay_);
    stage->addStretch(1);
    // The overlay hides between loops; a strut keeps its room so nothing jumps.
    stage->addStrut(overlay_->height());

    sourceLabel_ = new WrappedLabel(QString(), this);
    // Settings' secondary caption style.
    sourceLabel_->setObjectName(QStringLiteral("infoLabel"));
    micButton_ = neutralButton(new Button(QStringLiteral("Try with my microphone"), this));
    micButton_->setCheckable(true);
    connect(micButton_, &QAbstractButton::toggled, this, &RecordingLookPreview::onMicToggled);
    // Stacked, so the caption wraps at full width in a narrow window.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addLayout(stage);
    layout->addWidget(sourceLabel_);
    layout->addWidget(micButton_, 0, Qt::AlignLeft);
    // Only once parented: a parentless widget made visible becomes its own window and takes
    // activation from Settings.
    micButton_->setVisible(static_cast<bool>(recorderFactory_));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    timer_ = new QTimer(this);
    timer_->setInterval(kTickMs);
    connect(timer_, &QTimer::timeout, this, &RecordingLookPreview::tick);
    updateSourceLabel();
}

RecordingLookPreview::~RecordingLookPreview() {
    timer_->stop();
    heard_.disconnect();
    closeMicrophone();
}

std::int64_t RecordingLookPreview::steadyMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               clock_->steady().time_since_epoch())
        .count();
}

double RecordingLookPreview::elapsed() const {
    return static_cast<double>(steadyMs() - loopStartedMs_) / 1000.0;
}

void RecordingLookPreview::setLook(const QString& look) {
    overlay_->setRecordingLook(look);
    if (timer_->isActive()) {
        restart();
    }
}

void RecordingLookPreview::setLabelParts(bool dot, bool text, bool clock) {
    overlay_->setLabelParts(dot, text, clock);
}

void RecordingLookPreview::restart() {
    overlay_->hide();
    overlay_->showAtCursor(WaveformOverlay::kStateRecording);
    loopStartedMs_ = steadyMs();
    stage_ = kStages.front().name;
}

void RecordingLookPreview::tick() {
    if (micButton_->isChecked()) {
        return; // the microphone feeds the overlay as levels arrive
    }
    double at = elapsed();
    if (at >= kLoopS) {
        restart();
        at = 0.0;
    }
    std::string_view stage = kStages.front().name;
    for (const SampleStage& entry : kStages) {
        if (at >= entry.start) {
            stage = entry.name;
        }
    }
    if (stage != stage_) {
        stage_ = stage;
        if (stage == "processing") {
            overlay_->setState(WaveformOverlay::kStateProcessing);
        } else if (stage == "transcribing") {
            overlay_->setState(WaveformOverlay::kStateTranscribing);
        } else if (stage == "done") {
            overlay_->hide(); // fades out, as when the text lands
        }
    }
    if (stage == "recording") {
        overlay_->updateAudioLevels(QList<float>(20, static_cast<float>(sample_level(at))));
    }
}

void RecordingLookPreview::onHeard(float level) {
    if (micButton_->isChecked()) {
        overlay_->updateAudioLevels(QList<float>(20, level));
    }
}

void RecordingLookPreview::onMicToggled(bool on) {
    if (on && !openMicrophone()) {
        const QSignalBlocker blocker(micButton_);
        micButton_->setChecked(false);
        return;
    }
    if (!on) {
        closeMicrophone();
    }
    micButton_->setText(on ? QStringLiteral("Stop microphone")
                           : QStringLiteral("Try with my microphone"));
    updateSourceLabel();
    if (timer_->isActive()) {
        restart();
    }
}

bool RecordingLookPreview::openMicrophone() {
    std::string reason;
    try {
        recorder_ = recorderFactory_();
        if (recorder_ != nullptr) {
            // A level from the audio thread, delivered to the GUI thread; the subscription ends
            // before the recorder goes, so nothing arrives after the preview is gone.
            heard_ = recorder_->level.connect(*ui_, [this](float level) { onHeard(level); });
            if (recorder_->start_recording()) {
                return true;
            }
            reason = recorder_->last_start_error();
        }
    } catch (const std::exception& error) {
        // A missing or busy device must not break Settings.
        kLog.warning("Could not open the microphone for the recording look preview: {}",
                     error.what());
        reason = error.what();
    }
    releaseRecorder();
    sourceLabel_->setText(reason.empty() ? QStringLiteral("Couldn't open the microphone.")
                                         : QStringLiteral("Couldn't open the microphone: %1.")
                                               .arg(QString::fromStdString(reason)));
    return false;
}

void RecordingLookPreview::closeMicrophone() {
    if (recorder_ != nullptr) {
        try {
            recorder_->cancel_recording();
        } catch (const std::exception& error) {
            kLog.debug("Could not cancel the preview recording: {}", error.what());
        }
    }
    releaseRecorder();
}

void RecordingLookPreview::releaseRecorder() {
    heard_.disconnect();
    if (recorder_ != nullptr) {
        try {
            recorder_->cleanup();
        } catch (const std::exception& error) {
            kLog.debug("Could not clean up the preview recorder: {}", error.what());
        }
        recorder_.reset();
    }
}

void RecordingLookPreview::updateSourceLabel() {
    QString text;
    if (micButton_->isChecked()) {
        const QString name = microphoneName_ ? microphoneName_() : QString();
        const QString source = name.isEmpty() ? QStringLiteral("your microphone") : name;
        text = QStringLiteral("Listening to %1. Nothing is recorded.").arg(source);
    } else {
        text = QStringLiteral("Playing a sample voice.");
    }
    sourceLabel_->setText(text);
}

void RecordingLookPreview::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    restart();
    timer_->start();
}

void RecordingLookPreview::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    timer_->stop(); // first, so closing the microphone doesn't replay
    if (micButton_->isChecked()) {
        micButton_->setChecked(false); // closes the microphone
    }
    overlay_->hide();
}

std::filesystem::path RecordingLookPreview::previewPath() {
    std::error_code error;
    std::filesystem::path folder = std::filesystem::temp_directory_path(error);
    const std::string name = py::lower(identity().product) + "_overlay_preview.wav";
    return folder / name;
}

} // namespace fv
