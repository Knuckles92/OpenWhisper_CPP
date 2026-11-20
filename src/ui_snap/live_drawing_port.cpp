#include "ui_snap/live_drawing_port.h"

#include <QPoint>
#include <QString>

#include "ui_snap/snap_port.h"

namespace fv::ui_snap {

QtLiveDrawingPort::QtLiveDrawingPort(LiveAnnotations* live) : live_(live) {
    connectSignals();
}

QtLiveDrawingPort::QtLiveDrawingPort(QWidget* panel,
                                     const std::map<std::string, std::string>& markShortcuts,
                                     LiveAnnotations::Options options) {
    MarkShortcuts shortcuts;
    for (const auto& [action, keys] : markShortcuts) {
        shortcuts.insert(QString::fromStdString(action), QString::fromStdString(keys));
    }
    live_ = new LiveAnnotations(nullptr, panel, std::move(shortcuts), options);
    connectSignals();
}

QtLiveDrawingPort::~QtLiveDrawingPort() {
    close();
}

void QtLiveDrawingPort::connectSignals() {
    LiveAnnotations* live = live_;
    if (live == nullptr) {
        return;
    }
    connections_ = {
        QObject::connect(live, &LiveAnnotations::arming, [this] { arming.emit(); }),
        QObject::connect(live, &LiveAnnotations::captureChanged,
                         [this] { capture_changed.emit(); }),
        QObject::connect(live, &LiveAnnotations::drawn,
                         [this](QScreen* screen, const QList<Annotation>& marks) {
                             drawn.emit(screenRef(screen), marksPtr(std::vector<Annotation>(
                                                               marks.begin(), marks.end())));
                         }),
        QObject::connect(live, &LiveAnnotations::captureDelayChanged,
                         [this](int seconds) { capture_delay_changed.emit(seconds); }),
        QObject::connect(live, &LiveAnnotations::captureCancelled,
                         [this] { capture_cancelled.emit(); }),
        QObject::connect(live, &LiveAnnotations::interactionChanged,
                         [this](bool active) { interaction_changed.emit(active); }),
        QObject::connect(live, &LiveAnnotations::enabledChanged,
                         [this](bool enabled) { enabled_changed.emit(enabled); }),
    };
}

void QtLiveDrawingPort::arm() {
    if (live_) {
        live_->arm();
    }
}

void QtLiveDrawingPort::disarm() {
    if (live_) {
        live_->disarm();
    }
}

void QtLiveDrawingPort::set_enabled(bool enabled) {
    if (live_) {
        live_->setEnabled(enabled);
    }
}

void QtLiveDrawingPort::set_busy(bool busy) {
    if (live_) {
        live_->setBusy(busy);
    }
}

void QtLiveDrawingPort::set_capture_pending(bool pending, bool captured,
                                            std::optional<walkthrough::ScreenRef> screen) {
    if (!live_) {
        return;
    }
    std::optional<QScreen*> on;
    if (screen) {
        // A screen that has gone matches no canvas.
        on = screenFor(*screen);
    }
    live_->setCapturePending(pending, captured, on);
}

void QtLiveDrawingPort::set_capture_countdown(double remaining, double total) {
    if (live_) {
        live_->setCaptureCountdown(remaining, total);
    }
}

std::optional<walkthrough::CaptureRegion> QtLiveDrawingPort::capture_region() const {
    if (!live_) {
        return std::nullopt;
    }
    const std::optional<LiveRegion> region = live_->captureRegion();
    if (!region || !region->screen) {
        return std::nullopt;
    }
    const QRect rect = region->rect;
    const QRect global = rect.translated(region->screen->geometry().topLeft());
    return walkthrough::CaptureRegion{
        screenRef(region->screen),
        {rect.x(), rect.y(), rect.width(), rect.height()},
        {global.x(), global.y(), global.width(), global.height()},
    };
}

walkthrough::MarksPtr QtLiveDrawingPort::pinned_at(std::pair<int, int> position) const {
    if (!live_) {
        return nullptr;
    }
    return marksPtr(live_->pinnedAt(QPoint(position.first, position.second)));
}

walkthrough::MarksPtr
QtLiveDrawingPort::pinned_on(std::optional<walkthrough::ScreenRef> screen) const {
    if (!live_ || !screen) {
        return nullptr;
    }
    return marksPtr(live_->pinnedOn(screenFor(*screen)));
}

void QtLiveDrawingPort::handle_mark_shortcut(const std::string& action) {
    if (live_) {
        live_->handleMarkShortcut(action);
    }
}

bool QtLiveDrawingPort::interacting() const {
    return live_ && live_->interacting();
}

bool QtLiveDrawingPort::armed() const {
    return live_ && live_->armed();
}

void QtLiveDrawingPort::close() {
    // Closing hides the drawing first, and enabled_changed(false) still reaches the runtime:
    // that is what unregisters the mark shortcuts.
    if (LiveAnnotations* live = live_.data()) {
        live_ = nullptr;
        live->close();
    }
    for (const QMetaObject::Connection& connection : connections_) {
        QObject::disconnect(connection);
    }
    connections_.clear();
}

} // namespace fv::ui_snap
