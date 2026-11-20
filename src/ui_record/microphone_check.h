#pragma once

#include <functional>
#include <memory>

#include <QString>
#include <QWidget>

#include "base/executor.h"
#include "base/subscription.h"

class QProgressBar;
class QTimer;

namespace fv {

class Recorder;
class WrappedLabel;

// A short microphone test: a live level bar and a plain verdict. The microphone opens only while
// the test runs and closes on stop, after kListenMs, or when the widget leaves the screen.
// Nothing is kept: the take is cancelled. Levels arrive from the audio thread and are delivered
// to the GUI thread by a queued post, never inline. Affinity: GUI thread.
class MicrophoneCheck : public QWidget {
    Q_OBJECT
public:
    using RecorderFactory = std::function<std::unique_ptr<Recorder>()>;

    // LISTEN_MS: how long a test listens before it stops itself.
    static constexpr int kListenMs = 15000;
    // QUIET_MS: how long silence lasts before the verdict says nothing was heard.
    static constexpr int kQuietMs = 4000;
    // RMS levels: a quiet room reports about 0.003, speech at a desk 0.04 to 0.16.
    static constexpr double kFloor = 0.004;
    static constexpr double kHeard = 0.02;
    static constexpr double kFull = 0.2;

    // Builds the test; the microphone stays closed until start(). recorderFactory makes a
    // Recorder on the saved microphone (a private output file: it never records).
    explicit MicrophoneCheck(RecorderFactory recorderFactory, QWidget* parent = nullptr);
    ~MicrophoneCheck() override;

    bool isRunning() const { return recorder_ != nullptr; }

    // The bar for a level: round(100 * min(1, (level - floor) / (full - floor)) ** 0.5), 0 for a
    // level under the floor.
    static int barValue(double level);

    // Opens the microphone. False, with the reason shown, when it can't.
    bool start();
    // Closes the microphone, keeping the verdict on screen.
    void stop();
    // Shows a level now: what the queued delivery of the recorder's level does on the GUI
    // thread. Ignored while the test is not running.
    void applyLevel(float level);

    QProgressBar* bar() const { return bar_; }
    WrappedLabel* status() const { return status_; }
    // The two single-shot timers, for tests to expire without waiting.
    QTimer* stopTimer() const { return stopTimer_; }
    QTimer* quietTimer() const { return quietTimer_; }

Q_SIGNALS:
    void runningChanged(bool running);

protected:
    void hideEvent(QHideEvent* event) override;

private:
    // Cancels the running take so nothing of it is kept; a no-op when not running.
    void cancelTake();
    void release();
    void say(const QString& text, const QString& tone = {});
    void onQuiet();

    RecorderFactory recorderFactory_;
    std::unique_ptr<Recorder> recorder_;
    bool heard_ = false;
    // Declared before the subscription, which ends first.
    std::unique_ptr<Executor> levelQueue_;
    Subscription levelLink_;

    QProgressBar* bar_;
    WrappedLabel* status_;
    QTimer* stopTimer_;
    QTimer* quietTimer_;
};

} // namespace fv
