#pragma once

#include <cstdint>
#include <optional>

#include <QString>
#include <QWidget>

#include "remote/link.h"

class QLabel;

namespace fv {

// Where a remote engine's pass went, as "on jed · 31 ms network" (stats_display.py
// remote_timing_text); "" without a host. The network share shows only when the host timed every
// request it was sent (older hosts don't).
QString remoteTimingText(const std::optional<remote::RemoteTiming>& remote);

// The strip under the transcript: how long the transcription took, how long the audio was and how
// big the recording file is. Hidden until stats arrive. Affinity: GUI thread.
class TranscriptionStatsWidget : public QWidget {
    Q_OBJECT
public:
    explicit TranscriptionStatsWidget(QWidget* parent = nullptr);

    // Shows the strip with the three values. A file size that is unknown reads "--". The time of
    // the AI cleanup pass is not shown here (cleanupTime is accepted so every caller passes what
    // the runtime reports).
    // remote names the computer that did the work, under the time.
    void setStats(double transcriptionTime, double audioDuration,
                  std::optional<std::int64_t> fileSize, double cleanupTime = 0.0,
                  const std::optional<remote::RemoteTiming>& remote = std::nullopt);
    // Back to the "--" placeholders, hidden.
    void clear();

    QLabel* transcriptionTimeLabel() const { return transcriptionTimeValue_; }
    QLabel* audioDurationLabel() const { return audioDurationValue_; }
    QLabel* fileSizeLabel() const { return fileSizeValue_; }
    // Which computer did the work, under the time (#statsRemoteDetail); hidden without one.
    QLabel* remoteDetailLabel() const { return remoteDetail_; }

Q_SIGNALS:
    // Emitted by setStats (true) and clear (false), whether or not the state changed.
    void visibilityChanged(bool visible);

private:
    QWidget* createStatItem(const QString& labelText, const QString& valueText,
                            QLabel*& valueLabel);
    QWidget* createSeparator();

    QLabel* transcriptionTimeValue_ = nullptr;
    QLabel* audioDurationValue_ = nullptr;
    QLabel* fileSizeValue_ = nullptr;
    QLabel* remoteDetail_ = nullptr;
};

} // namespace fv
