#include "ui_snap/session.h"

#include <stdexcept>
#include <utility>

#include <QCoreApplication>
#include <QCursor>
#include <QEventLoop>
#include <QGuiApplication>
#include <QScreen>
#include <QStringList>
#include <QThread>

#include "base/log.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_snap/capture.h"
#include "ui_snap/export.h"
#include "ui_snap/overlay.h"
#include "ui_snap/pointed.h"
#include "ui_snap/support.h"

namespace fv::ui_snap {
namespace {

constexpr log::Logger kLog{"ui_qt.snap.session"};

// Sessions still waiting on their outcome. Holding them here lets a caller write
// startSnap()->kept(...) without keeping the session.
QList<SnapSession*>& live() {
    static QList<SnapSession*> sessions;
    return sessions;
}

// Runs a function when the scope ends, however it ends: Python's try/finally.
template <class F> class Finally {
public:
    explicit Finally(F fn) : fn_(std::move(fn)) {}
    ~Finally() { fn_(); }
    Finally(const Finally&) = delete;
    Finally& operator=(const Finally&) = delete;

private:
    F fn_;
};

void unpark(std::vector<std::pair<Parkable, bool>>& windows) {
    for (const auto& [window, guarded] : windows) {
        // A window given as a QObject that has been deleted is not called back.
        if (guarded && window.window.isNull()) {
            continue;
        }
        if (window.unpark) {
            window.unpark();
        }
    }
    windows.clear();
}

} // namespace

struct SnapSession::Frozen {
    QScreen* screen;
    QImage image;
    QList<QRect> targets;
};

SnapSession::SnapSession(QObject* parent, SettingsStore* settings)
    : QObject(parent), settings_(settings) {
    settle_.setSingleShot(true);
    connect(&settle_, &QTimer::timeout, this, &SnapSession::capture);
    notice_.setSingleShot(true);
    notice_.setInterval(0);
    connect(&notice_, &QTimer::timeout, this, &SnapSession::emitFailure);
}

SnapSession::~SnapSession() {
    live().removeAll(this);
    // Should an owner delete the session mid-snap, nothing is left hidden or parked, and no
    // overlay outlives it to cover the screen for good.
    restoreWindows(hidden_);
    hidden_.clear();
    unparkAll();
    for (const QPointer<SnapOverlay>& overlay : std::as_const(overlays_)) {
        if (overlay) {
            overlay->deleteLater();
        }
    }
}

QList<SnapSession*> SnapSession::liveSessions() {
    return live();
}

QList<SnapOverlay*> SnapSession::overlays() const {
    QList<SnapOverlay*> open;
    for (const QPointer<SnapOverlay>& overlay : overlays_) {
        if (overlay) {
            open.append(overlay);
        }
    }
    return open;
}

SnapOverlay* SnapSession::overlay() const {
    const QList<SnapOverlay*> open = overlays();
    if (open.isEmpty()) {
        return nullptr;
    }
    for (SnapOverlay* overlay : open) {
        if (overlay->selection()) {
            return overlay;
        }
    }
    QScreen* pointed = QGuiApplication::screenAt(QCursor::pos());
    for (SnapOverlay* overlay : open) {
        if (overlay->snapScreen() == pointed) {
            return overlay;
        }
    }
    return open.front();
}

void SnapSession::keep() {
    if (decided_) {
        return;
    }
    if (SnapOverlay* open = overlay()) {
        open->keep();
    } else {
        keepWhenCaptured_ = true;
    }
}

void SnapSession::drop() {
    if (decided_) {
        return;
    }
    if (SnapOverlay* open = overlay()) {
        open->drop();
        return;
    }
    settle_.stop();
    // macOS's native capture would be cancelled here; it is not ported.
    restoreHidden();
    finish([this] { Q_EMIT dropped(); });
}

// ---- capture ----

void SnapSession::start(SnapOptions options) {
    const auto [supported, reason] = snapSupport();
    if (!supported) {
        failSoon(reason);
        return;
    }
    QList<QScreen*> screens;
    if (options.screen != nullptr) {
        if (QGuiApplication::screens().contains(options.screen)) {
            screens.append(options.screen);
        }
    } else if (!options.clickAt) {
        screens = screensToCover();
    } else if (QScreen* clicked = screenAtClick(*options.clickAt)) {
        screens.append(clicked);
    }
    if (screens.isEmpty()) {
        failSoon(QString::fromUtf16(kNoScreen));
        return;
    }
    draw_ = options.take == SnapTake::Draw;
    if (options.take != SnapTake::Region) {
        screens = screens.mid(0, 1);
        keepWhenCaptured_ = !draw_;
        if (options.take == SnapTake::Window) {
            crop_ = windowUnderPointer(screens.front(), options.keepOut, options.front);
        }
    }
    if (!keepWhenCaptured_) {
        for (QScreen* screen : std::as_const(screens)) {
            windowTargets_.append(windowTargets(screen, options.keepOut));
        }
    }
    for (QScreen* screen : std::as_const(screens)) {
        screens_.append(screen);
    }
    hint_ = options.hint;
    floatAbove_ = std::move(options.floatAbove);
    // macOS remembers the focused application here and filters its automatic captures
    // natively (macos.remember_focus, macos_capture); neither is ported.
    hidden_.append(hideWindows(options.keepOut));
    if (hidden_.isEmpty() && !keepWhenCaptured_) {
        capture();
        return;
    }
    // Let the hide reach the window system, then give the compositor a couple of frames to stop
    // drawing those windows before grabbing.
    if (!options.clickAt && options.screen == nullptr) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
    if (!decided_) {
        // A keep reports from the event loop, after startSnap returns.
        settle_.start(hidden_.isEmpty() ? 0 : settleMs(screens_.front()));
    }
}

void SnapSession::capture() {
    if (decided_) {
        return;
    }
    QList<std::optional<QImage>> images;
    {
        const Finally restore([this] { restoreHidden(); });
        for (const QPointer<QScreen>& screen : std::as_const(screens_)) {
            // A screen removed since the snap started has nothing to grab (Python's grab of a
            // deleted QScreen raises, which is a failed capture).
            images.append(screen ? grab(screen) : std::nullopt);
        }
    }
    captured(images);
}

void SnapSession::captured(const QList<std::optional<QImage>>& images) {
    if (!images.front()) {
        failSoon(QString::fromUtf16(kCaptureFailed));
    } else if (keepWhenCaptured_) {
        const QImage whole = crop_ ? renderKept(*images.front(), *crop_) : *images.front();
        const QRect area = screens_.front()->geometry();
        captureRect_ = crop_ ? crop_->translated(area.topLeft()) : area;
        finish([this, whole] { Q_EMIT kept(whole); });
    } else {
        // Another screen that couldn't be captured is left uncovered.
        std::vector<Frozen> frozen;
        for (int index = 0; index < screens_.size(); ++index) {
            if (images[index]) {
                frozen.push_back({screens_[index], *images[index], windowTargets_.value(index)});
            }
        }
        open(frozen);
    }
}

void SnapSession::restoreHidden() {
    restoreWindows(hidden_);
    hidden_.clear();
}

void SnapSession::open(const std::vector<Frozen>& frozen) {
    const bool snapEnabled = settings_ != nullptr ? settings_->get(keys::snap_to_edges)
                                                  : keys::snap_to_edges.read(Json::object());
    QStringList parts;
    if (!hint_.isEmpty()) {
        parts.append(hint_);
    }
    parts.append(QStringLiteral("S toggles snapping"));
    const QString hint = parts.join(QStringLiteral(u" · "));
    for (const Frozen& item : frozen) {
        auto* overlay = new SnapOverlay(item.image, item.screen,
                                        {.hint = hint,
                                         .snapEnabled = snapEnabled,
                                         .targets = item.targets,
                                         .settings = settings_});
        connect(overlay, &SnapOverlay::kept, this,
                [this, overlay](const QImage& image) { onKept(overlay, image); });
        connect(overlay, &SnapOverlay::dropped, this,
                [this] { finish([this] { Q_EMIT dropped(); }); });
        connect(overlay, &SnapOverlay::selecting, this, [this, overlay] { onSelecting(overlay); });
        connect(overlay, &SnapOverlay::snapToggled, this, &SnapSession::setSnapEnabled);
        connect(overlay, &SnapOverlay::boxInfillToggled, this, &SnapSession::setBoxInfill);
        overlays_.append(overlay);
    }
    // The pointer's screen opens last, so it has the focus for Enter and Esc.
    for (auto it = overlays_.crbegin(); it != overlays_.crend(); ++it) {
        if (*it) {
            (*it)->present();
        }
    }
    if (draw_ && !overlays_.isEmpty() && overlays_.front()) {
        overlays_.front()->startDrawing();
    }
    const QRect area = screens_.front()->geometry();
    for (Parkable& window : floatAbove_) {
        if (window.park) {
            window.park(area);
        }
        parked_.emplace_back(window, !window.window.isNull());
    }
}

void SnapSession::setSnapEnabled(bool enabled) {
    if (settings_ != nullptr) {
        settings_->save_setting(keys::snap_to_edges.key(), enabled);
    }
    for (SnapOverlay* overlay : overlays()) {
        overlay->setSnapEnabled(enabled);
    }
}

void SnapSession::setBoxInfill(bool enabled) {
    for (SnapOverlay* overlay : overlays()) {
        overlay->setBoxInfill(enabled);
    }
}

void SnapSession::onSelecting(SnapOverlay* chosen) {
    // The selection moved to chosen's screen: clear the rest, focus it.
    for (SnapOverlay* overlay : overlays()) {
        if (overlay != chosen && overlay->selection()) {
            overlay->clearSelection();
        }
    }
    if (!chosen->isActiveWindow()) {
        chosen->activateWindow();
        chosen->setFocus();
    }
}

// ---- outcome ----

void SnapSession::onKept(SnapOverlay* overlay, const QImage& image) {
    const QRect from = overlay->selection().value_or(overlay->rect());
    captureRect_ = from.translated(overlay->pos());
    finish([this, image] { Q_EMIT kept(image); });
}

void SnapSession::failSoon(const QString& reason) {
    // Fails now, but reports from the event loop: never inside startSnap.
    kLog.info("Snap failed: {}", reason.toStdString());
    decided_ = true;
    failure_ = reason;
    notice_.start();
}

void SnapSession::emitFailure() {
    // A receiver may delete the session or its owner; the destructor then leaves the live set.
    const QPointer<SnapSession> alive(this);
    const Finally done([this, &alive] {
        if (alive) {
            live().removeAll(this);
            if (parent() == nullptr) {
                deleteLater();
            }
        }
    });
    Q_EMIT failed(failure_);
}

void SnapSession::finish(const std::function<void()>& report) {
    // Hides the overlays first: the live screen is back while callers handle the outcome.
    if (decided_) {
        return;
    }
    decided_ = true;
    const QList<QPointer<SnapOverlay>> overlays = std::exchange(overlays_, {});
    for (const QPointer<SnapOverlay>& overlay : overlays) {
        if (overlay) {
            overlay->hide();
        }
    }
    unparkAll();
    // macOS gives the focus back to the application that had it here (not ported).
    // A receiver may delete the session or its owner; the destructor then leaves the live set.
    const QPointer<SnapSession> alive(this);
    const Finally done([this, &alive, &overlays] {
        for (const QPointer<SnapOverlay>& overlay : overlays) {
            if (overlay) {
                overlay->deleteLater();
            }
        }
        if (alive) {
            live().removeAll(this);
            if (parent() == nullptr) {
                deleteLater();
            }
        }
    });
    report();
}

void SnapSession::unparkAll() {
    unpark(parked_);
}

SnapSession* startSnap(SnapOptions options) {
    const QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr || QThread::currentThread() != app->thread()) {
        throw std::logic_error("startSnap must be called on the Qt thread");
    }
    auto* session = new SnapSession(options.parent, options.settings);
    live().append(session);
    session->start(std::move(options));
    return session;
}

} // namespace fv::ui_snap
