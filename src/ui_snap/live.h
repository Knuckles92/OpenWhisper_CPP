#pragma once

#include <chrono>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <QList>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QScreen>
#include <QTimer>
#include <QWidget>

#include "ui_snap/annotations.h"
#include "ui_snap/capture_delay.h"
#include "ui_snap/overlay.h"
#include "ui_snap/toolbar.h"

namespace fv {
class Clock;
class SettingsStore;
} // namespace fv

// Drawing on the live desktop while a drawing tool is selected (ui_qt/snap/live.py): Auto Mode's
// floating toolbar and a transparent canvas per screen. Unarmed, the canvases let every click
// through and only show the marks still alive; armed, they take the pointer (a 1-alpha fill on
// Windows, which passes clicks through fully transparent pixels) and keep the focus. A finished
// mark is reported with every mark of its canvas, so the walkthrough runtime can screenshot it;
// it then fades 1.5 s later unless pinned (Ctrl at press or commit) or Keep marks is on. Drawing
// here has no undo or redo. Affinity: GUI thread.
namespace fv::ui_snap {

class LiveAnnotations;

// How long a temporary mark stays on the desktop, in seconds.
inline constexpr double kMarkLifetimeS = 1.5;
inline constexpr std::chrono::milliseconds kMarkLifetime(1500);

// The region badge on the canvases while a capture region is being dragged.
inline constexpr char16_t kRegionHint[] = u"Drag a capture region · Esc to cancel";

// Auto Mode's capture region: a screen and a rectangle in its own logical pixels.
struct LiveRegion {
    QPointer<QScreen> screen;
    QRect rect;
};

// One screen's transparent drawing surface (Python's _Canvas).
class LiveCanvas : public QWidget {
    Q_OBJECT

public:
    LiveCanvas(LiveAnnotations* live, QScreen* screen);

    // The screen this canvas covers (QWidget::screen() is where its window is now).
    QScreen* canvasScreen() const { return screen_; }
    // Window frames the arrow, box and ellipse snap to; the canvas itself first.
    void setTargets(QList<QRect> targets) { targets_ = std::move(targets); }
    const QList<QRect>& targets() const { return targets_; }

    // Shows the canvas as the live drawing is now: taking input while armed, click-through and
    // hidden when it has nothing to show otherwise.
    void present();
    // Its marks in drawing order; pinnedOnly leaves out those that will fade.
    std::vector<Annotation> items(bool pinnedOnly = false) const;
    void add(Annotation mark, bool pinned);
    // Drops the marks whose time has come and schedules the next; held while a delayed
    // screenshot is pending.
    void expireMarks();
    // Every mark gone and step numbers from 1 again.
    void clear();
    // Drops the mark or label being made, and any region being dragged.
    void cancelDraft();
    // A capture that kept this screen: only the pinned marks stay.
    void keepPinnedOnly();
    // A capture that did not happen: temporary marks get a fresh lifetime from now.
    void renewTemporary();
    // The capture region when it is on this screen.
    std::optional<QRect> region() const;

    LabelEditor* labelEditor() const { return labelEditor_; }
    bool drafting() const { return draft_.has_value(); }
    int nextStep() const { return nextStep_; }
    QTimer& expireTimer() { return expire_; }

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    struct Mark {
        Annotation mark;
        // nullopt: pinned, it stays until Clear.
        std::optional<std::chrono::steady_clock::time_point> expires;
    };

    void geometryChanged(const QRect& rect);
    QPointF point(QPointF point) const;
    void openLabel(QPointF point, const QColor& color, bool pin);
    void commitLabel();

    LiveAnnotations* live_;
    QPointer<QScreen> screen_;
    QList<QRect> targets_;
    std::vector<Mark> marks_;
    int nextStep_ = 1;
    std::optional<Annotation> draft_;
    std::optional<QPointF> regionStart_;
    std::optional<QRect> regionDraft_;
    QPointer<LabelEditor> labelEditor_;
    bool pinLabel_ = false;
    QTimer expire_;
};

// The 24 x 24 × above a capture region's top-right: the only part of the region that takes
// input; it closes the region, so automatic screenshots take the whole screen again.
class LiveRegionClose : public QWidget {
    Q_OBJECT

public:
    explicit LiveRegionClose(LiveAnnotations* live);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    LiveAnnotations* live_;
    bool pressed_ = false;
};

// One toolbar per walkthrough, with temporary drawing input on every screen.
class LiveAnnotations : public QObject {
    Q_OBJECT

public:
    struct Options {
        // Where snap_box_infill, snap_to_edges, walkthrough_keep_marks and
        // walkthrough_capture_delay are read and saved; nullptr reads the defaults, saves nothing
        // and keeps toggles in memory.
        SettingsStore* settings = nullptr;
        // The time marks fade by; nullptr is the system clock.
        const Clock* clock = nullptr;
    };

    // panel is the walkthrough's column, raised with the toolbar and left out of the edges
    // marks snap to. markShortcuts name the Keep marks and Clear marks shortcuts in their tips.
    // The toolbar shows at once on the screen under the pointer.
    // Two constructors, not a default argument: GCC cannot value-initialise Options, whose
    // members have initialisers, before the enclosing class is complete.
    explicit LiveAnnotations(QObject* parent = nullptr, QWidget* panel = nullptr,
                             MarkShortcuts markShortcuts = {});
    LiveAnnotations(QObject* parent, QWidget* panel, MarkShortcuts markShortcuts, Options options);
    ~LiveAnnotations() override;

    // A tool is in hand or a region is being dragged.
    bool armed() const { return tool_.has_value() || selectingRegion_; }
    bool busy() const { return busy_; }
    bool enabled() const { return enabled_; }
    std::optional<SnapTool> tool() const { return tool_; }
    bool selectingRegion() const { return selectingRegion_; }
    std::optional<LiveRegion> captureRegion() const { return captureRegion_; }
    int color() const { return color_; }
    bool boxInfill() const { return boxInfill_; }
    bool snapEnabled() const { return snapEnabled_; }
    bool keepMarks() const { return keepMarks_; }
    // A stroke is being drawn or a label typed on some canvas.
    bool interacting() const;

    // Every window of the live drawing, to keep out of screenshots: the toolbar, the canvases,
    // the region's ×, the delay picker.
    QList<QWidget*> windows() const;
    SnapToolbar* toolbar() const { return toolbar_; }
    CaptureDelay* captureDelay() const { return captureDelay_; }
    LiveCanvas* canvas(QScreen* screen) const;
    QList<LiveCanvas*> canvases() const;
    LiveRegionClose* regionClose() const { return regionClose_; }
    const Clock& clock() const { return *clock_; }

    // The delay chip's countdown: seconds left of total, 0 when no screenshot is coming.
    void setCaptureCountdown(double remaining, double total = 0.0);
    // A delayed screenshot is coming (pending), or came (captured) or was dropped. screen is
    // the monitor it was of: marks drawn on another one were not in it, so they stay; nullopt is
    // every monitor (a nullptr inside matches none).
    void setCapturePending(bool pending, bool captured = false,
                           std::optional<QScreen*> screen = std::nullopt);
    // × and Esc: cancels a screenshot that is coming and puts the tool down.
    void cancelCapture();
    // Shows (true) or hides the toolbar and canvases; hiding disarms.
    void setEnabled(bool enabled);
    void toggleTool(SnapTool tool);
    void selectRegion();
    void arm(SnapTool tool = SnapTool::Pen);
    void disarm();
    void setColor(int color);
    void toggleSnap();
    void toggleBoxInfill();
    void toggleKeepMarks();
    // A mark shortcut: "keep_marks" or "clear"; ignored while hidden.
    void handleMarkShortcut(std::string_view action);
    // A screenshot is being taken: no drawing meanwhile.
    void setBusy(bool busy);
    // The region dragged on screen, in its logical pixels; under 4 px either way is ignored.
    void setRegion(QScreen* screen, QRect rect);
    void clearRegion();
    // Disarms once the focus has left every canvas.
    void checkFocus();
    // A mark finished on canvas.
    void complete(LiveCanvas* canvas, const Annotation& mark, bool pinned);
    // The pinned marks of the canvas under a native click position (screenAtClick), and of
    // screen's canvas.
    std::vector<Annotation> pinnedAt(QPoint position) const;
    std::vector<Annotation> pinnedOn(QScreen* screen) const;
    // Clear marks: every canvas, and any screenshot that is coming.
    void clear();
    // Hides everything and deletes the windows and then this object (deleteLater).
    void close();

Q_SIGNALS:
    void arming();
    void drawn(QScreen* screen, const QList<fv::ui_snap::Annotation>& marks);
    void enabledChanged(bool enabled);
    void captureChanged();
    void captureDelayChanged(int seconds);
    void captureCancelled();
    void interactionChanged(bool active);

private:
    friend class LiveCanvas;

    // nullopt: the capture region.
    void armFor(std::optional<SnapTool> tool);
    void syncDrop();
    void syncToolbar();
    void showToolbar();
    void showRegionClose();
    void raiseControls();
    bool canvasHasFocus() const;
    void removeScreen(QScreen* screen);
    bool capturePending() const { return capturePending_; }

    struct Surface {
        QPointer<QScreen> screen;
        QPointer<LiveCanvas> canvas;
    };

    SettingsStore* settings_ = nullptr;
    const Clock* clock_ = nullptr;
    QPointer<QWidget> panel_;
    std::vector<Surface> canvases_;
    // Canvases of removed screens, deleted later but also here: a LiveAnnotations deleted
    // directly (not through close()) before the event loop runs must not leave one holding a
    // dangling live_ (its expiry timer reads it).
    QList<QPointer<LiveCanvas>> retired_;
    bool enabled_ = false;
    bool busy_ = false;
    bool capturePending_ = false;
    bool countingDown_ = false;
    std::optional<SnapTool> tool_;
    bool selectingRegion_ = false;
    std::optional<LiveRegion> captureRegion_;
    QPointer<LiveRegionClose> regionClose_;
    int color_ = 0;
    bool boxInfill_ = false;
    bool snapEnabled_ = false;
    bool keepMarks_ = false;
    WId previousFocus_ = 0;
    QPointer<QScreen> screen_;
    QPointer<SnapToolbar> toolbar_;
    QPointer<CaptureDelay> captureDelay_;
};

} // namespace fv::ui_snap
