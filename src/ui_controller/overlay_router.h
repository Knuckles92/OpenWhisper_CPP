#pragma once

#include <functional>

#include <QObject>
#include <QPointer>
#include <QString>

#include "appcore/overlay_state.h"

class QTimer;

namespace fv {

class SystemTray;
class UploadPresenter;
class WaveformOverlay;

// config.CANCELLATION_ANIMATION_DURATION_MS and CANCELLATION_GRACE_MS: the Canceled moment plays
// for 1200 ms, and the overlay hides 200 ms after it.
inline constexpr int kCancellationAnimationMs = 1200;
inline constexpr int kCancellationGraceMs = 200;
// The overlay's level meter takes 20 bars (streaming.setup_audio_level_callback: [level] * 20).
inline constexpr int kOverlayLevelBars = 20;

// Where an overlay state goes (ui_controller.py:875-1000, 1436-1471): the one place that decides
// between the recording overlay, the streaming preview and hiding everything, and keeps the
// tray's recording item and Quick Record's activity line in step. A job the Upload File tab
// started reports on that tab's card instead (setUploadPresenter), except its end.
// Affinity: GUI thread.
class OverlayRouter : public QObject {
    Q_OBJECT
public:
    explicit OverlayRouter(WaveformOverlay& overlay, QObject* parent = nullptr);

    // Quick Record's set_activity_state; empty: nothing to tell.
    std::function<void(appcore::OverlayState)> activityState;
    // The tray, once Bootstrap made it.
    void setTray(SystemTray* tray);
    // Where an upload job's stages go (_upload_job_active, _route_upload_progress); null: none.
    void setUploadPresenter(UploadPresenter* upload);

    // set_overlay_state: an upload job's stage goes to its card, the overlay hidden; CANCELING
    // turns the tray item off and plays the cancel animation; NONE turns it off and hides the
    // overlay and the streaming preview; RECORDING shows the listening overlay; Processing,
    // Transcribing, Cleaning and Rewriting dismiss the preview text and show that state; STT
    // Enabled and Disabled play their moment.
    void setOverlayState(appcore::OverlayState state);
    // show_large_file_state: an upload job's card says it, otherwise the overlay near the
    // pointer. `fileSizeMb` is MiB.
    void showLargeFileState(double fileSizeMb);
    // show_streaming_overlay, update_streaming_text and hide_streaming_overlay: the live
    // preview's text on the overlay. Showing marks the streaming flow active before the
    // recording's Listening state arrives, so the waveform never flashes first.
    void showStreamingOverlay();
    void updateStreamingText(const QString& text, bool final);
    void hideStreamingOverlay();
    // The microphone's level, 0..1, as the 20 bars the overlay draws.
    void setLevel(float level);
    void showTranscriptionRetry(const QString& label);
    // The remote engine's reconnects ran out: the notice shows whatever window is in front.
    void showTranscriptionRetryFailure(const QString& label);
    // A shortcut or tray outcome near the pointer, skipped while the main window is active.
    void showNotice(const QString& text, bool done, bool mainWindowActive);
    void showCopiedAnimation();
    void showCaption(const QString& text);
    void setHandsFree(bool on);
    void setRecordingDestination(const QString& destination);
    void hideOverlay();
    // showStreamingOverlay turns this on while the preview shows text on the overlay.
    void setStreamingFlowActive(bool active) { streamingFlowActive_ = active; }
    bool streamingFlowActive() const { return streamingFlowActive_; }
    QTimer& cancelAnimationTimer() const { return *cancelAnimationTimer_; }
    WaveformOverlay& overlay() const { return overlay_; }
    // Stops the cancel timer and closes the overlay (UIController.cleanup).
    void cleanup();

private:
    void setTrayRecording(bool recording);
    void showListeningOverlay(appcore::OverlayState state);
    void dismissStreamingPreviewForWaveform(const QString& waveformState);
    void showOrSetOverlay(const QString& overlayState);
    void startCancelAnimation();
    void onCancelAnimationFinished();

    WaveformOverlay& overlay_;
    QPointer<SystemTray> tray_;
    QPointer<UploadPresenter> upload_;
    QTimer* cancelAnimationTimer_;
    bool streamingFlowActive_ = false;
};

} // namespace fv
