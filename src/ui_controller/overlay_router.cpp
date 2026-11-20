#include "ui_controller/overlay_router.h"

#include <QList>
#include <QTimer>

#include "base/log.h"
#include "ui_overlay/waveform_overlay.h"
#include "ui_record/upload_presenter.h"
#include "ui_shell/system_tray.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.ui_controller"};

} // namespace

OverlayRouter::OverlayRouter(WaveformOverlay& overlay, QObject* parent)
    : QObject(parent), overlay_(overlay), cancelAnimationTimer_(new QTimer(this)) {
    cancelAnimationTimer_->setSingleShot(true);
    connect(cancelAnimationTimer_, &QTimer::timeout, this,
            &OverlayRouter::onCancelAnimationFinished);
}

void OverlayRouter::setTray(SystemTray* tray) {
    tray_ = tray;
}

void OverlayRouter::setUploadPresenter(UploadPresenter* upload) {
    upload_ = upload;
}

void OverlayRouter::setTrayRecording(bool recording) {
    if (tray_ != nullptr) {
        tray_->setRecording(recording);
    }
}

// ui_controller.py:875-914.
void OverlayRouter::setOverlayState(appcore::OverlayState state) {
    using appcore::OverlayState;
    // An upload job reports on its card; its NONE reaches the card too, then the teardown below.
    if (upload_ != nullptr && upload_->isUploadJobActive() && upload_->routeProgress(state)) {
        setTrayRecording(false);
        hideOverlay();
        return;
    }
    if (activityState) {
        activityState(state);
    }

    if (state == OverlayState::Canceling) {
        setTrayRecording(false);
        startCancelAnimation();
        return;
    }
    if (state == OverlayState::None) {
        setTrayRecording(false);
        hideOverlay();
        hideStreamingOverlay();
        return;
    }
    switch (state) {
    case OverlayState::Recording:
    case OverlayState::CommandListening:
        // Listening for a Command Mode instruction is a recording to the tray.
        setTrayRecording(true);
        showListeningOverlay(state);
        break;
    case OverlayState::Processing:
        setTrayRecording(false);
        overlay_.setProcessingLabel(QString());
        dismissStreamingPreviewForWaveform(WaveformOverlay::kStateProcessing);
        break;
    case OverlayState::Transcribing:
        setTrayRecording(false);
        dismissStreamingPreviewForWaveform(WaveformOverlay::kStateTranscribing);
        break;
    case OverlayState::Cleaning:
        setTrayRecording(false);
        dismissStreamingPreviewForWaveform(WaveformOverlay::kStateCleaning);
        break;
    case OverlayState::Rewriting:
        setTrayRecording(false);
        dismissStreamingPreviewForWaveform(WaveformOverlay::kStateRewriting);
        break;
    case OverlayState::SttEnabled:
        showOrSetOverlay(WaveformOverlay::kStateSttEnable);
        break;
    case OverlayState::SttDisabled:
        showOrSetOverlay(WaveformOverlay::kStateSttDisable);
        break;
    case OverlayState::None:
    case OverlayState::Canceling:
        break;
    }
}

// streaming.setup_audio_level_callback: each level fills every bar.
void OverlayRouter::setLevel(float level) {
    overlay_.updateAudioLevels(QList<float>(kOverlayLevelBars, level));
}

// ui_controller.py:961-970.
void OverlayRouter::showLargeFileState(double fileSizeMb) {
    if (upload_ != nullptr && upload_->showLargeFileState(fileSizeMb)) {
        return;
    }
    overlay_.setLargeFileInfo(fileSizeMb);
    overlay_.showAtCursor(WaveformOverlay::kStateLargeFileSplitting);
}

// ui_controller.py:1436-1448.
void OverlayRouter::showStreamingOverlay() {
    streamingFlowActive_ = true;
    overlay_.clearStreamingText();
    overlay_.showAtCursor(WaveformOverlay::kStateStreaming);
    kLog.debug("Streaming preview shown on waveform overlay");
}

void OverlayRouter::updateStreamingText(const QString& text, bool final) {
    overlay_.updateStreamingText(text, final);
}

void OverlayRouter::hideStreamingOverlay() {
    streamingFlowActive_ = false;
    overlay_.clearStreamingText();
    kLog.debug("Streaming preview cleared");
}

void OverlayRouter::showTranscriptionRetry(const QString& label) {
    dismissStreamingPreviewForWaveform(WaveformOverlay::kStateProcessing);
    overlay_.setProcessingLabel(label);
}

// ui_controller.py show_transcription_retry_failure: unlike showNotice, never skipped.
void OverlayRouter::showTranscriptionRetryFailure(const QString& label) {
    overlay_.showNotice(label);
}

// ui_controller.py:992-999: the status line already says it while the window is in front.
void OverlayRouter::showNotice(const QString& text, bool done, bool mainWindowActive) {
    if (!mainWindowActive) {
        overlay_.showNotice(text, done);
    }
}

void OverlayRouter::showCopiedAnimation() {
    overlay_.showAtCursor(WaveformOverlay::kStateCopied);
}

void OverlayRouter::showCaption(const QString& text) {
    overlay_.showCaption(text);
}

void OverlayRouter::setHandsFree(bool on) {
    overlay_.setHandsFree(on);
}

void OverlayRouter::setRecordingDestination(const QString& destination) {
    overlay_.setRecordingDestination(destination);
}

void OverlayRouter::hideOverlay() {
    overlay_.hide();
}

// ui_controller.py:923-932.
void OverlayRouter::showListeningOverlay(appcore::OverlayState state) {
    if (state == appcore::OverlayState::CommandListening) {
        showOrSetOverlay(WaveformOverlay::kStateCommandListening);
    } else if (streamingFlowActive_) {
        if (!overlay_.isVisible()) {
            overlay_.showAtCursor(WaveformOverlay::kStateStreaming);
        } else if (overlay_.currentState() != WaveformOverlay::kStateStreaming) {
            overlay_.setState(WaveformOverlay::kStateStreaming);
        }
    } else {
        showOrSetOverlay(WaveformOverlay::kStateRecording);
    }
}

// _dismiss_streaming_preview_for_waveform: a work state replaces the preview's text.
void OverlayRouter::dismissStreamingPreviewForWaveform(const QString& waveformState) {
    streamingFlowActive_ = false;
    overlay_.clearStreamingText();
    showOrSetOverlay(waveformState);
}

void OverlayRouter::showOrSetOverlay(const QString& overlayState) {
    if (!overlay_.isVisible()) {
        overlay_.showAtCursor(overlayState);
    } else {
        overlay_.setState(overlayState);
    }
}

// ui_controller.py:1450-1462.
void OverlayRouter::startCancelAnimation() {
    cancelAnimationTimer_->stop();
    streamingFlowActive_ = false;
    overlay_.clearStreamingText();
    showOrSetOverlay(WaveformOverlay::kStateCanceling);
    cancelAnimationTimer_->start(kCancellationAnimationMs + kCancellationGraceMs);
}

// ui_controller.py:1464-1471: a recording started meanwhile keeps its overlay.
void OverlayRouter::onCancelAnimationFinished() {
    const QString& state = overlay_.currentState();
    if (state != WaveformOverlay::kStateCanceling && state != WaveformOverlay::kStateIdle) {
        return;
    }
    hideOverlay();
    if (activityState) {
        activityState(appcore::OverlayState::None);
    }
}

void OverlayRouter::cleanup() {
    cancelAnimationTimer_->stop();
    if (overlay_.timer().isActive()) {
        overlay_.timer().stop();
    }
    overlay_.close();
}

} // namespace fv
