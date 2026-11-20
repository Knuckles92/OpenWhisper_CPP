#pragma once

#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include <QImage>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QScreen>
#include <QString>
#include <QTimer>
#include <QWidget>

namespace fv {
class SettingsStore;
}

// One snap from hotkey to kept image: capture, overlay, outcome (ui_qt/snap/session.py).
// startSnap grabs every screen before any of our UI appears, then opens an overlay on each, so
// the user can select on whichever screen holds what they want. Asked for the whole screen or
// the window under the pointer instead, it grabs the pointer's screen and keeps that, or the
// window's part of it, with no overlay. The session it returns reports exactly one outcome
// (kept, dropped or failed), never from inside startSnap, so callers connect after it returns.
// Affinity: GUI thread.
namespace fv::ui_snap {

class SnapOverlay;

inline constexpr char16_t kCaptureFailed[] = u"Couldn't capture the screen. Try again in a moment.";
inline constexpr char16_t kNoScreen[] = u"There's no screen to capture.";

// What a snap keeps: a region drawn on the overlay, or at once, the whole screen under the
// pointer or the window under it; Draw opens the overlay ready to draw on the whole screen.
enum class SnapTake { Region, Screen, Window, Draw };

// A window that stays in sight above an open snap, out of the way: park moves it into a corner
// of an area (global coordinates) above the overlay, unpark puts it back. When window is given
// and has been deleted by then, unpark is not called.
struct Parkable {
    std::function<void(QRect)> park;
    std::function<void()> unpark;
    QPointer<QObject> window;
};

struct SnapOptions {
    // Our windows that must not appear in the shot. Those kept out with keepOutOfScreenshots stay
    // on screen; the others are hidden around the grab, then shown again.
    QList<QPointer<QWidget>> keepOut;
    // Shown in the pill at the top, before "Enter to keep · Esc to cancel".
    QString hint;
    // Windows that stay in sight while the overlay is open, parked in a corner above it and put
    // back once the snap ends.
    std::vector<Parkable> floatAbove;
    // The session's owner; without one the session deletes itself after its outcome.
    QObject* parent = nullptr;
    SnapTake take = SnapTake::Region;
    // For Window, keep the window in front whatever the pointer is on: the snap was asked for
    // from our own windows. A pointer on one of keepOut means the same.
    bool front = false;
    // For an automatic Screen capture on Windows, the native coordinates of the click that
    // triggered it.
    std::optional<QPoint> clickAt;
    // For a live annotation, the screen the mark was drawn on.
    QScreen* screen = nullptr;
    // Where snap_to_edges and snap_box_infill are read and saved; nullptr reads the defaults and
    // saves nothing.
    SettingsStore* settings = nullptr;
};

// A snap in progress. Emits exactly one of kept, dropped or failed. keep() and drop() act like
// Enter and Esc and emit before they return; keep() while the capture is still pending emits
// once the screen has been captured. After the outcome isActive() is false and the overlays are
// hidden and deleted. A session with a parent lives as long as its parent; one without deletes
// itself (deleteLater) once its outcome has been emitted, so it is valid while its signal's
// receivers run. One selection at a time: starting one on a screen clears any other.
class SnapSession : public QObject {
    Q_OBJECT

public:
    ~SnapSession() override;

    // True while the capture is pending or the overlays are open.
    bool isActive() const { return !decided_; }
    // Where the kept image came from, in global logical coordinates; nullopt before a keep.
    std::optional<QRect> captureRect() const { return captureRect_; }
    // As Enter: keeps the selection, or the whole screen when there is none.
    void keep();
    // As Esc (always the whole snap, even while a label is being typed).
    void drop();

    // The open overlays, the pointer's screen first; empty before they open and after the
    // outcome.
    QList<SnapOverlay*> overlays() const;
    // Where a keep comes from: the overlay with a selection, else the pointer's screen's, else the
    // first; nullptr when none is open.
    SnapOverlay* overlay() const;

    // Sessions still waiting on their outcome (Python's module-level _live).
    static QList<SnapSession*> liveSessions();

Q_SIGNALS:
    // The annotated crop in device pixels; its devicePixelRatio is the screen's.
    void kept(const QImage& image);
    // The user cancelled: Esc, the Cancel button, or the overlay was closed.
    void dropped();
    // Capture failed or isn't supported here; a short user-facing reason.
    void failed(const QString& reason);

private:
    friend SnapSession* startSnap(SnapOptions options);

    SnapSession(QObject* parent, SettingsStore* settings);

    void start(SnapOptions options);
    void capture();
    void captured(const QList<std::optional<QImage>>& images);
    void restoreHidden();
    struct Frozen;
    void open(const std::vector<Frozen>& frozen);
    void setSnapEnabled(bool enabled);
    void setBoxInfill(bool enabled);
    void onSelecting(SnapOverlay* chosen);
    void onKept(SnapOverlay* overlay, const QImage& image);
    void failSoon(const QString& reason);
    void emitFailure();
    void finish(const std::function<void()>& report);
    void unparkAll();

    SettingsStore* settings_ = nullptr;
    QList<QPointer<SnapOverlay>> overlays_;
    // Guarded: a screen unplugged while the settle timer runs reads as one that failed to grab.
    QList<QPointer<QScreen>> screens_;
    QList<QList<QRect>> windowTargets_;
    QString hint_;
    bool draw_ = false;
    QList<QPointer<QWidget>> hidden_;
    std::vector<Parkable> floatAbove_;
    // Parked windows, each with whether it named a window to check for deletion.
    std::vector<std::pair<Parkable, bool>> parked_;
    bool decided_ = false;
    bool keepWhenCaptured_ = false;
    // The window to keep, in the pointer's screen's logical pixels.
    std::optional<QRect> crop_;
    std::optional<QRect> captureRect_;
    QString failure_;
    // Owned timers: they die with the session, never firing at a dead one.
    QTimer settle_;
    QTimer notice_;
};

// Freezes the screens and opens an overlay on each (or keeps at once, by options.take). GUI
// thread only: anywhere else it throws std::logic_error. Connect to the session's signals right
// away; one outcome follows, always after this function has returned.
SnapSession* startSnap(SnapOptions options = {});

} // namespace fv::ui_snap
