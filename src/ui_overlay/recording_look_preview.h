#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

#include <QString>
#include <QWidget>

#include "base/subscription.h"

class QTimer;

namespace fv {

class Button;
class Clock;
class Recorder;
class SettingsStore;
class WaveformOverlay;
class WrappedLabel;

namespace qt {
class QtUiExecutor;
}

// A live recording overlay inside Settings, so a look can be judged as it is set
// (ui_qt/widgets/recording_look_preview.py). It plays a sample voice on a loop: two phrases, a
// pause and a breath of silence at the levels a desk microphone reports, then the look's own
// finish (Processing, Transcribing, the fade out) before it starts again. "Try with my
// microphone" swaps in the user's own voice and stays recording; the microphone opens only
// while that is on and the preview is shown, and the take is thrown away.
// Affinity: GUI thread. Recorder levels arrive from its audio thread and are handed over.
class RecordingLookPreview : public QWidget {
    Q_OBJECT

public:
    // The tick of the sample voice (ms).
    static constexpr int kTickMs = 33;

    // recorderFactory makes the recorder for the microphone toggle (it should record to
    // previewPath()); empty hides the toggle, and a factory that returns null or throws is a
    // microphone that will not open. microphoneName names the microphone that will be used.
    // settings is what the overlay reads as the real one does (the saved look and label at
    // first, the dictation languages for its chip as each loop starts); null reads as an empty
    // settings file. clock defaults to the system clock. Both must outlive the preview. Nothing
    // animates until the preview is shown.
    explicit RecordingLookPreview(std::function<std::unique_ptr<Recorder>()> recorderFactory = {},
                                  std::function<QString()> microphoneName = {},
                                  QWidget* parent = nullptr,
                                  const SettingsStore* settings = nullptr,
                                  const Clock* clock = nullptr);
    ~RecordingLookPreview() override;

    // Shows look and replays the overlay's entry.
    void setLook(const QString& look);
    // Shows or hides the live dot, the "Recording" text and the clock.
    void setLabelParts(bool dot, bool text, bool clock);

    WaveformOverlay& overlay() const { return *overlay_; }
    WrappedLabel& sourceLabel() const { return *sourceLabel_; }
    Button& micButton() const { return *micButton_; }
    QTimer& timer() const { return *timer_; }
    bool microphoneOpen() const { return recorder_ != nullptr; }

    // Where the throwaway take spools while the microphone is on: a file named after the
    // product in the temporary folder.
    static std::filesystem::path previewPath();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    friend struct RecordingLookPreviewTestAccess;

    std::int64_t steadyMs() const;
    double elapsed() const;
    void restart();
    void tick();
    void onHeard(float level);
    void onMicToggled(bool on);
    bool openMicrophone();
    void closeMicrophone();
    void releaseRecorder();
    void updateSourceLabel();

    std::function<std::unique_ptr<Recorder>()> recorderFactory_;
    std::function<QString()> microphoneName_;
    const Clock* clock_;
    std::unique_ptr<qt::QtUiExecutor> ui_;
    std::unique_ptr<Recorder> recorder_;
    Subscription heard_;
    WaveformOverlay* overlay_;
    WrappedLabel* sourceLabel_;
    Button* micButton_;
    QTimer* timer_;
    // When the loop started, on the clock's steady time in whole milliseconds, as QElapsedTimer
    // counts.
    std::int64_t loopStartedMs_ = 0;
    std::string_view stage_;
};

} // namespace fv
