#include "ui_record/microphone_check.h"

#include <algorithm>
#include <cmath>
#include <exception>

#include <QPointer>
#include <QProgressBar>
#include <QTimer>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "base/log.h"
#include "recorder/recorder.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.microphone_check"};

// Hands every callback to the GUI thread by a queued post, whichever thread emits, so a level is
// never applied inside the audio thread's callback (or inline on the GUI thread). A callback
// that arrives after the widget is gone is dropped.
class QueuedToGui final : public Executor {
public:
    explicit QueuedToGui(QObject* context) : context_(context) {}

    void post(Task task) override { qt::post(context_, std::move(task)); }
    TimerHandle post_after(std::chrono::milliseconds, Task) override { return {}; }
    bool is_current() const override { return false; }

private:
    QPointer<QObject> context_;
};

} // namespace

MicrophoneCheck::MicrophoneCheck(RecorderFactory recorderFactory, QWidget* parent)
    : QWidget(parent), recorderFactory_(std::move(recorderFactory)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    bar_ = new QProgressBar;
    bar_->setObjectName(QStringLiteral("microphoneCheckBar"));
    bar_->setRange(0, 100);
    bar_->setTextVisible(false);
    bar_->setFixedHeight(8);
    bar_->setAccessibleName(QStringLiteral("Microphone level"));
    layout->addWidget(bar_);

    status_ = new WrappedLabel(QString());
    status_->setTextFormat(Qt::PlainText);
    status_->setObjectName(QStringLiteral("guidedDetail"));
    layout->addWidget(status_);

    stopTimer_ = new QTimer(this);
    stopTimer_->setSingleShot(true);
    stopTimer_->setInterval(kListenMs);
    connect(stopTimer_, &QTimer::timeout, this, [this] { stop(); });
    quietTimer_ = new QTimer(this);
    quietTimer_->setSingleShot(true);
    quietTimer_->setInterval(kQuietMs);
    connect(quietTimer_, &QTimer::timeout, this, [this] { onQuiet(); });

    levelQueue_ = std::make_unique<QueuedToGui>(this);
}

MicrophoneCheck::~MicrophoneCheck() {
    // The microphone closes with the widget even when nobody called stop(), and the take is
    // cancelled first, as stop() does: cleanup() alone keeps an unacknowledged take on disk for
    // recovery, and the check never records.
    levelLink_.disconnect();
    cancelTake();
    release();
}

int MicrophoneCheck::barValue(double level) {
    const double shown = std::max(0.0, level - kFloor) / (kFull - kFloor);
    // Python's round(): half to even, which std::nearbyint does in the default rounding mode.
    return static_cast<int>(std::nearbyint(100.0 * std::sqrt(std::min(1.0, shown))));
}

bool MicrophoneCheck::start() {
    stop();
    heard_ = false;
    bar_->setValue(0);
    bool started = false;
    QString reason;
    try {
        recorder_ = recorderFactory_();
        if (recorder_ == nullptr) {
            // Python reaches its except branch here (None has no level callback); the reason it
            // would show is about None, not the microphone, so the plain text is shown instead.
            kLog.warning("The microphone check's recorder factory made no recorder");
        } else {
            levelLink_ =
                recorder_->level.connect(*levelQueue_, [this](float level) { applyLevel(level); });
            started = recorder_->start_recording();
            if (!started) {
                reason = QString::fromStdString(recorder_->last_start_error());
            }
        }
    } catch (const std::exception& error) {
        // A missing or busy device must not break setup.
        kLog.warning("Could not open the microphone for the microphone check: {}", error.what());
        started = false;
        reason = QString::fromUtf8(error.what());
    }
    if (!started) {
        release();
        say(reason.isEmpty()
                ? QStringLiteral("Couldn't open the microphone. Pick another one, or check that "
                                 "%1 is allowed to use it.")
                      .arg(qt::toQString(identity().product))
                : QStringLiteral("Couldn't open the microphone: %1.").arg(reason),
            QStringLiteral("attention"));
        return false;
    }
    say(QStringLiteral("Listening. Say something and watch the bar. Nothing is recorded."));
    stopTimer_->start();
    quietTimer_->start();
    Q_EMIT runningChanged(true);
    return true;
}

void MicrophoneCheck::stop() {
    if (recorder_ == nullptr) {
        return;
    }
    stopTimer_->stop();
    quietTimer_->stop();
    cancelTake();
    release();
    bar_->setValue(0);
    if (!heard_) {
        say(QStringLiteral("Didn't hear anything. Check that the microphone isn't muted, or pick "
                           "another one."),
            QStringLiteral("attention"));
    }
    Q_EMIT runningChanged(false);
}

void MicrophoneCheck::cancelTake() {
    if (recorder_ == nullptr) {
        return;
    }
    try {
        recorder_->cancel_recording();
    } catch (const std::exception& error) {
        kLog.debug("Could not cancel the microphone check recording: {}", error.what());
    }
}

void MicrophoneCheck::release() {
    levelLink_.disconnect();
    std::unique_ptr<Recorder> recorder = std::move(recorder_);
    if (recorder != nullptr) {
        try {
            recorder->cleanup();
        } catch (const std::exception& error) {
            kLog.debug("Could not clean up the microphone check recorder: {}", error.what());
        }
    }
}

void MicrophoneCheck::say(const QString& text, const QString& tone) {
    status_->setText(text);
    ui::setStyleProperty(status_, ui::prop::tone, tone);
}

void MicrophoneCheck::applyLevel(float level) {
    if (recorder_ == nullptr) {
        return;
    }
    bar_->setValue(barValue(level));
    if (level >= kHeard && !heard_) {
        heard_ = true;
        quietTimer_->stop();
        say(QStringLiteral("Hearing you. This microphone works."), QStringLiteral("ready"));
    }
}

void MicrophoneCheck::onQuiet() {
    if (recorder_ != nullptr && !heard_) {
        say(QStringLiteral("Not hearing anything yet. Check that the microphone isn't muted, or "
                           "pick another one."),
            QStringLiteral("attention"));
    }
}

void MicrophoneCheck::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    stop();
}

} // namespace fv
