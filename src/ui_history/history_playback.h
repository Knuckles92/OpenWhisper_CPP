#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <QLabel>
#include <QObject>
#include <QPointer>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include "history/entry.h"

class QTimer;

// Play and stop a History entry's recording, with a thin progress line
// (ui_qt/widgets/history_playback.py). The player (audio/player.h) knows nothing about Qt: this
// side polls it on an owned timer while one of its recordings plays, so no audio thread ever
// reaches a widget, and a card rebuilt by a refresh picks up the playback it shows.
namespace fv {

class Executor;
namespace qt {
class QtUiExecutor;
}

namespace playback {

inline constexpr char kRecordingBusy[] = "Playback is off while you're recording";
inline constexpr char kMeetingBusy[] = "Playback is off during a meeting";
// Retention removed the file; the entry still says it had one.
inline constexpr char kRecordingRemoved[] = "This recording was removed to save space";
inline constexpr char kNotOnHost[] = "The host didn't keep this recording";
inline constexpr char kCantPlay[] = "This recording can't be played";

// stop_all: stops any playback, and any click still waiting for a host download. Affinity: any
// thread.
void stopAll();
// Forgets the host recordings fetched this session and the downloads under way (a test's fresh
// start, as OpenWhisper's tests reset _fetched and _in_flight). Affinity: GUI thread.
void forgetHostRecordings();

// "0:04", "12:30": whole seconds, negative counting as 0.
std::string clock(double seconds);

// Why nothing should play now, from the windows widget lives in: playback would leak into a
// recording or a meeting's capture. A dialog looks through its parent window. A window answers
// through an invokable isRecording() (MainWindow's own when it is one) and meetingIsActive().
// Affinity: GUI thread.
QString busyReason(const QWidget* widget);

} // namespace playback

// A thin line that fills as the recording plays. Affinity: GUI thread.
class PlaybackProgress : public QWidget {
    Q_OBJECT
public:
    explicit PlaybackProgress(QWidget* parent = nullptr);

    double fraction() const { return fraction_; }
    // Clamped to 0..1; repaints only for a change of at least 0.001 or at either end.
    void setFraction(double fraction);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    double fraction_ = 0.0;
};

// Play/Stop for one entry, driving a button, a progress line and a time label. Owned by the
// card or dialog that shows them, which lays the three out; the line and the label show only
// while this entry plays. A host-kept entry's recording is fetched from the host first (M9):
// until then its button only shows whether the host kept one.
// Affinity: GUI thread.
class PlaybackControl : public QObject {
    Q_OBJECT
public:
    static constexpr int kPollMs = 50;

    using BusyFn = std::function<QString()>;

    // recordingPath: the entry's saved recording when the file exists (HistoryStore's
    // get_recording_path). busy defaults to playback::busyReason(button).
    PlaybackControl(const HistoryEntry& entry, std::optional<std::filesystem::path> recordingPath,
                    QPushButton* button, QObject* parent, BusyFn busy = {});
    ~PlaybackControl() override;

    QPushButton* button() const { return button_; }
    PlaybackProgress* progress() const { return progress_; }
    QLabel* timeLabel() const { return timeLabel_; }
    bool available() const { return available_; }
    // The last refusal or failure shown to the user.
    const QString& note() const { return note_; }

    // How a host-kept recording is fetched (record sync's audio_for), on `io`. One download per
    // entry runs at a time, shared by every control showing it; what arrived is kept for the
    // session, and the click that asked plays it in whichever control shows that entry then.
    void setRemoteAudio(Executor&, std::function<std::filesystem::path()>);
    bool isPlaying() const;
    // Fetching the host-kept recording now ("Getting it…").
    bool isFetching() const { return fetching_; }
    void toggle();
    // Stops this entry's playback, if it is the one playing or about to.
    void stop();
    // take_over: a card rebuilt in place becomes the one a pending click on `old` plays in.
    void takeOver(PlaybackControl* old);
    // _sync: follows the player once; the timer calls it every kPollMs while playing.
    void sync();

Q_SIGNALS:
    // true when this entry starts playing, false when it stops.
    void playingChanged(bool playing);

private:
    void fetch();
    static void fetchFinished(const std::string& id, const std::filesystem::path& path,
                              const QString& error);
    void onFetched(const std::filesystem::path& path, const QString& error, bool asked);
    void play(const std::filesystem::path& path);
    void say(const QString& message);
    void setPlaying();
    void setIdle();
    void setButtonText(const QString& text);

    QPushButton* button_;
    PlaybackProgress* progress_;
    QLabel* timeLabel_;
    BusyFn busy_;
    std::uint64_t session_ = 0;
    QString note_;
    std::optional<std::filesystem::path> path_;
    Executor* io_ = nullptr;
    std::function<std::filesystem::path()> fetch_;
    std::string entryId_;
    bool fetching_ = false;
    bool available_ = false;
    bool hostKept_ = false;
    QString unavailable_;
    QTimer* timer_;
};

} // namespace fv
