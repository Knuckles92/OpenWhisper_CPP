#include "ui_history/history_playback.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <map>
#include <set>
#include <vector>

#include <QCoreApplication>
#include <QMetaMethod>
#include <QPainter>
#include <QPainterPath>
#include <QPoint>
#include <QRectF>
#include <QSizePolicy>
#include <QThread>
#include <QTimer>
#include <QToolTip>

#include "appkit/qt_bridge.h"
#include "audio/player.h"
#include "base/desktop_session.h"
#include "base/log.h"
#include "ui_shell/main_window.h"
#include "ui_theme/icons.h"
#include "ui_theme/palette.h"
#include "ui_widgets/buttons.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.history_playback"};

QIcon icon(const char* name) {
    return ui::tablerIcon(QString::fromLatin1(name) + ui::currentPalette().qCss(u"icon-suffix") +
                          QStringLiteral(".svg"));
}

// A bool-returning invokable method the window declares, as Python's getattr found an
// attribute; nullopt when it declares none.
std::optional<bool> askWindow(QWidget* window, const char* signature, const char* name) {
    const QMetaObject* meta = window->metaObject();
    if (meta->indexOfMethod(signature) < 0) {
        return std::nullopt;
    }
    bool answer = false;
    if (!QMetaObject::invokeMethod(window, name, Qt::DirectConnection,
                                   Q_RETURN_ARG(bool, answer))) {
        return std::nullopt;
    }
    return answer;
}

// history_playback.py _fetched and _in_flight. GUI thread only.
struct InFlight {
    // The control whose click plays the download, once it arrives; a null pointer while
    // `wanted` means that control was deleted (a refresh rebuilt its card).
    QPointer<PlaybackControl> waiting;
    bool wanted = false;
};

// Where each host-kept entry's recording was fetched to this session, so a card rebuilt by a
// refresh still knows its playback.
std::map<std::string, std::filesystem::path>& fetched() {
    static std::map<std::string, std::filesystem::path> paths;
    return paths;
}

// Host-kept entries downloading now.
std::map<std::string, InFlight>& inFlight() {
    static std::map<std::string, InFlight> downloads;
    return downloads;
}

// The controls alive now; a finished download is offered to each of its entry's.
std::set<PlaybackControl*>& liveControls() {
    static std::set<PlaybackControl*> controls;
    return controls;
}

// _FetchRelay: lives with the application, so a download never reaches a control a refresh
// deleted, and a worker never holds or emits on one.
QObject* relay() {
    static QPointer<QObject> object;
    if (object == nullptr) {
        object = new QObject(QCoreApplication::instance());
    }
    return object;
}

bool onGuiThread() {
    return QCoreApplication::instance() != nullptr &&
           QThread::currentThread() == QCoreApplication::instance()->thread();
}

void forgetWaiting() {
    for (auto& [id, download] : inFlight()) {
        download.waiting = nullptr;
        download.wanted = false;
    }
}

} // namespace

namespace playback {

void stopAll() {
    audio::stop_playback();
    // Any click still waiting for a host download no longer wants it.
    if (onGuiThread()) {
        forgetWaiting();
    } else if (QCoreApplication::instance() != nullptr) {
        QMetaObject::invokeMethod(relay(), [] { forgetWaiting(); }, Qt::QueuedConnection);
    }
}

void forgetHostRecordings() {
    fetched().clear();
    inFlight().clear();
}

std::string clock(double seconds) {
    // int(seconds) truncates toward zero; NaN is not a time a player reports.
    const long long whole = std::max(0LL, static_cast<long long>(std::trunc(seconds)));
    return std::format("{}:{:02}", whole / 60, whole % 60);
}

QString busyReason(const QWidget* widget) {
    QWidget* window = widget != nullptr ? widget->window() : nullptr;
    std::set<const QWidget*> seen;
    while (window != nullptr && !seen.contains(window)) {
        seen.insert(window);
        bool recording = false;
        if (const auto* main = qobject_cast<const MainWindow*>(window)) {
            recording = main->isRecording();
        } else {
            recording = askWindow(window, "isRecording()", "isRecording").value_or(false);
        }
        if (recording) {
            return QString::fromUtf8(kRecordingBusy);
        }
        try {
            if (askWindow(window, "meetingIsActive()", "meetingIsActive").value_or(false)) {
                return QString::fromUtf8(kMeetingBusy);
            }
        } catch (const std::exception& error) {
            kLog.debug("Could not ask whether a meeting is running: {}", error.what());
        }
        QWidget* parent = window->parentWidget();
        window = parent != nullptr ? parent->window() : nullptr;
    }
    return {};
}

} // namespace playback

PlaybackProgress::PlaybackProgress(QWidget* parent) : QWidget(parent) {
    setFixedHeight(4);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void PlaybackProgress::setFraction(double fraction) {
    fraction = std::min(1.0, std::max(0.0, fraction));
    if (std::abs(fraction - fraction_) >= 0.001 || fraction == 0.0 || fraction == 1.0) {
        fraction_ = fraction;
        update();
    }
}

void PlaybackProgress::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF area(rect());
    const double radius = use_omarchy_ui() ? 1.0 : area.height() / 2;
    QPainterPath clip;
    clip.addRoundedRect(area, radius, radius);
    painter.setClipPath(clip);
    painter.fillRect(area, ui::tokenColor("overlay-rgb", 28));
    const QRectF filled(area.left(), area.top(), area.width() * fraction_, area.height());
    painter.fillRect(filled, ui::tokenColor("accent"));
    painter.end();
}

PlaybackControl::PlaybackControl(const HistoryEntry& entry,
                                 std::optional<std::filesystem::path> recordingPath,
                                 QPushButton* button, QObject* parent, BusyFn busy)
    : QObject(parent), button_(button), progress_(new PlaybackProgress),
      timeLabel_(new QLabel(QString())), busy_(std::move(busy)), timer_(new QTimer(this)) {
    timeLabel_->setObjectName(QStringLiteral("historyPlayTime"));
    if (!busy_) {
        busy_ = [this] {
            return playback::busyReason(button_);
        };
    }
    entryId_ = entry.id;
    if (entry.stored_on && !entry.stored_on->empty()) {
        hostKept_ = true;
        available_ = entry.remote_audio;
        unavailable_ = QString::fromUtf8(playback::kNotOnHost);
        if (const auto known = fetched().find(entry.id); known != fetched().end()) {
            path_ = known->second;
        }
        if (const auto download = inFlight().find(entry.id); download != inFlight().end()) {
            fetching_ = true;
            if (download->second.wanted && download->second.waiting == nullptr) {
                // A refresh rebuilt the clicked card; this one shows it now.
                download->second.waiting = this;
            }
        }
        liveControls().insert(this);
        (void)relay(); // made here, on the GUI thread, before any download reports to it
    } else if (entry.audio_file && !entry.audio_file->empty()) {
        path_ = std::move(recordingPath);
        available_ = path_.has_value();
        unavailable_ = QString::fromUtf8(playback::kRecordingRemoved);
    }

    timer_->setInterval(kPollMs);
    connect(timer_, &QTimer::timeout, this, &PlaybackControl::sync);
    connect(button_, &QPushButton::clicked, this, &PlaybackControl::toggle);
    setIdle();
    if (path_) {
        const std::shared_ptr<audio::Player> current = audio::player();
        if (current->is_playing() && current->path() == *path_) {
            session_ = current->session();
            setPlaying();
        }
    }
}

PlaybackControl::~PlaybackControl() {
    liveControls().erase(this);
}

void PlaybackControl::setRemoteAudio(Executor& io, std::function<std::filesystem::path()> fetch) {
    io_ = &io;
    fetch_ = std::move(fetch);
}

bool PlaybackControl::isPlaying() const {
    return session_ != 0 && audio::player()->session() == session_;
}

void PlaybackControl::toggle() {
    if (isPlaying()) {
        stop();
        return;
    }
    if (!available_ || fetching_) {
        return;
    }
    const QString reason = busy_();
    if (!reason.isEmpty()) {
        say(reason);
        return;
    }
    if (hostKept_ && !path_) {
        fetch();
        return;
    }
    play(*path_);
}

void PlaybackControl::takeOver(PlaybackControl* old) {
    const auto download = inFlight().find(entryId_);
    if (old != this && download != inFlight().end() && download->second.wanted &&
        download->second.waiting == old) {
        download->second.waiting = this;
    }
}

void PlaybackControl::stop() {
    if (const auto download = inFlight().find(entryId_);
        download != inFlight().end() && download->second.waiting == this) {
        download->second.waiting = nullptr;
        download->second.wanted = false;
    }
    if (isPlaying()) {
        audio::player()->stop();
    }
    setIdle();
}

void PlaybackControl::fetch() {
    if (!fetch_ || io_ == nullptr) {
        say(QString::fromUtf8(playback::kCantPlay));
        return;
    }
    const bool downloading = inFlight().contains(entryId_);
    inFlight()[entryId_] = {this, true};
    fetching_ = true;
    setIdle();
    if (downloading) {
        // Another control's download is under way; a second one would fetch the same file.
        return;
    }
    io_->post([id = entryId_, fetch = fetch_] {
        std::filesystem::path path;
        QString error;
        try {
            path = fetch();
        } catch (const std::exception& problem) {
            kLog.warning("Could not fetch a recording from the host: {}", problem.what());
            const std::string reason = problem.what();
            error = qt::toQString(reason.empty() ? "error" : reason);
        }
        QMetaObject::invokeMethod(
            relay(), [id, path, error] { PlaybackControl::fetchFinished(id, path, error); },
            Qt::QueuedConnection);
    });
}

void PlaybackControl::fetchFinished(const std::string& id, const std::filesystem::path& path,
                                    const QString& error) {
    if (!path.empty() && error.isEmpty()) {
        fetched()[id] = path;
    }
    PlaybackControl* waiting = nullptr;
    if (const auto download = inFlight().find(id); download != inFlight().end()) {
        waiting = download->second.wanted ? download->second.waiting.data() : nullptr;
        inFlight().erase(download);
    }
    const std::vector<PlaybackControl*> controls(liveControls().begin(), liveControls().end());
    for (PlaybackControl* control : controls) {
        if (liveControls().contains(control) && control->entryId_ == id) {
            control->onFetched(path, error, waiting == control);
        }
    }
}

void PlaybackControl::onFetched(const std::filesystem::path& path, const QString& error,
                                bool asked) {
    if (!path.empty() && error.isEmpty()) {
        path_ = path;
    }
    if (fetching_) {
        fetching_ = false;
        setIdle();
    }
    if (!asked) {
        return;
    }
    if (!error.isEmpty() || path.empty()) {
        say(error.isEmpty() ? QString::fromUtf8(playback::kCantPlay)
                            : QStringLiteral("Couldn't get the recording: %1").arg(error));
        return;
    }
    const QString reason = busy_();
    if (!reason.isEmpty()) {
        say(reason);
        return;
    }
    play(path);
}

void PlaybackControl::play(const std::filesystem::path& path) {
    const std::shared_ptr<audio::Player> player = audio::player();
    if (path.empty() || !player->play(path)) {
        say(QString::fromUtf8(playback::kCantPlay));
        return;
    }
    session_ = player->session();
    setPlaying();
}

void PlaybackControl::say(const QString& message) {
    note_ = message;
    QToolTip::showText(button_->mapToGlobal(QPoint(0, button_->height())), message, button_);
}

void PlaybackControl::setButtonText(const QString& text) {
    // Button refits its width to a new label only through its own setText.
    if (auto* fitted = qobject_cast<Button*>(button_)) {
        fitted->setText(text);
    } else {
        button_->setText(text);
    }
}

void PlaybackControl::setPlaying() {
    setButtonText(QStringLiteral("Stop"));
    button_->setIcon(icon("player-stop-danger"));
    button_->setToolTip(QStringLiteral("Stop playing"));
    progress_->show();
    timeLabel_->show();
    sync();
    timer_->start();
    Q_EMIT playingChanged(true);
}

void PlaybackControl::setIdle() {
    const bool wasPlaying = timer_->isActive();
    timer_->stop();
    session_ = 0;
    setButtonText(fetching_ ? QStringLiteral("Getting it…") : QStringLiteral("Play"));
    button_->setIcon(icon("player-play-gray"));
    button_->setEnabled(available_ && !fetching_);
    button_->setToolTip(available_ ? QStringLiteral("Play the recording") : unavailable_);
    progress_->setFraction(0.0);
    progress_->hide();
    timeLabel_->hide();
    if (wasPlaying) {
        Q_EMIT playingChanged(false);
    }
}

void PlaybackControl::sync() {
    const std::shared_ptr<audio::Player> player = audio::player();
    if (session_ == 0 || player->session() != session_) {
        setIdle();
        return;
    }
    const double duration = player->duration();
    const double position = std::min(player->position(), duration);
    progress_->setFraction(duration != 0.0 ? position / duration : 0.0);
    timeLabel_->setText(
        QString::fromStdString(playback::clock(position) + " / " + playback::clock(duration)));
}

} // namespace fv
