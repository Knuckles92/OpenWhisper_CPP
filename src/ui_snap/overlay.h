#pragma once

#include <optional>

#include <QColor>
#include <QImage>
#include <QLineEdit>
#include <QList>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QRegion>
#include <QSize>
#include <QString>
#include <QWidget>

#include "ui_snap/annotations.h"
#include "ui_snap/chrome.h"
#include "ui_snap/geometry.h"
#include "ui_theme/palette.h"

class QScreen;

namespace fv {
class SettingsStore;
}

// The full-screen freeze-and-annotate overlay (ui_qt/snap/overlay.py). It shows the frozen
// screen under a smoked layer. The user drags out a region, which shows through clear with its
// size beside it, then marks it up with the toolbar that appears under it; its corners and edges
// drag to adjust it. Marks are pinned to the screen, not to the region, so moving an edge never
// moves them: the region only decides what is kept. Enter keeps, Esc drops. The overlay emits
// kept (the annotated crop, device pixels) or dropped exactly once; whoever opened it closes and
// deletes it.
namespace fv::ui_snap {

class SnapToolbar;

// A drag smaller than this either way is a click, not a selection.
inline constexpr int kMinSelection = 6;

enum class SnapPhase { Select, Annotate };

// A frameless top-level window that covers one screen. Wayland offers neither stay-on-top nor
// positioning, so there it is a plain toplevel that asks for fullscreen. On Windows it is an
// always-on-top tool window, which keeps it off the taskbar; on macOS a tool window (a panel)
// keeps the Dock icon from appearing for it. X11 window managers only fullscreen normal windows
// reliably, so there it stays a normal window kept on top. Decided from systemPlatform() and
// whether the Qt platform is Wayland.
Qt::WindowFlags snapWindowFlags();
Qt::WindowFlags snapWindowFlags(const std::string& system, bool wayland);

// The label's top-left, centred vertically on the click and inside the drawing area.
QPoint labelOrigin(const QRect& area, QPointF point, QSize size);

// The inline editor for a text label, styled as the tag it becomes. Enter commits the label and
// Esc cancels only the label: both are swallowed here so they never reach the overlay as keep
// or drop. Affinity: GUI thread.
class LabelEditor : public QLineEdit {
    Q_OBJECT

public:
    static constexpr int kMaxLength = 200;

    LabelEditor(QWidget* parent, const QColor& color);

    QColor color() const { return color_; }
    void setColor(const QColor& color);
    // Sizes to the text again, now that the editor has its place.
    void refit();
    // The label this editor makes: at its position, its text stripped, in its colour and font.
    Annotation annotation() const;

Q_SIGNALS:
    void committed();
    void cancelled();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    // Tab stays here (see keyPressEvent): the overlay treats every letter as a tool shortcut.
    bool focusNextPrevChild(bool next) override;

private:
    // Grows with the text, but never past the parent's right edge (it scrolls).
    void fit();

    QColor color_;
};

// Freeze, select, annotate: one screen, one snap. Affinity: GUI thread.
class SnapOverlay : public QWidget {
    Q_OBJECT

public:
    struct Options {
        // Shown in the pill at the top, before "Enter to keep · Esc to cancel".
        QString hint;
        bool snapEnabled = false;
        // Window frames the selection snaps to, in the screen's logical pixels; the whole
        // screen is always one.
        QList<QRect> targets;
        // Where snap_box_infill is read and saved; nullptr reads the default and saves nothing.
        SettingsStore* settings = nullptr;
    };

    // frozen carries its device-pixel ratio; screen is where present() shows the overlay.
    // Two constructors, not a default argument: GCC cannot value-initialise Options, whose
    // members have initialisers, before the enclosing class is complete.
    explicit SnapOverlay(const QImage& frozen, QScreen* screen = nullptr);
    SnapOverlay(const QImage& frozen, QScreen* screen, const Options& options);
    ~SnapOverlay() override;

    // ---- what callers and tests read ----
    SnapPhase phase() const { return phase_; }
    std::optional<QRect> selection() const { return selection_; }
    const AnnotationStack& annotations() const { return stack_; }
    SnapTool tool() const { return tool_; }
    int colorIndex() const { return color_; }
    LabelEditor* labelEditor() const { return editor_; }
    SnapToolbar* toolbar() const { return toolbar_; }
    // The screen this overlay covers (Python's _screen); nullptr when none was given.
    QScreen* snapScreen() const { return screen_; }
    const QImage& frozen() const { return frozen_; }
    bool snapEnabled() const { return snapEnabled_; }
    bool boxInfill() const { return boxInfill_; }
    QColor currentColor() const;
    // The hint pill's place: top centre, else bottom centre, never over the selection or the
    // toolbar; empty when neither is free.
    QRect hintRect() const;

    // ---- showing, keeping, dropping ----
    // Covers the captured screen, focused so that Enter and Esc reach it.
    void present();
    // Keeps the selection (the whole screen when there is none) with its marks.
    void keep();
    void drop();
    // Selects the whole screen and starts annotating it.
    void selectAll();
    // Starts a whole-screen annotation with the pen ready to draw.
    void startDrawing();
    // Back to the bare frozen screen: the selection moved to another screen.
    void clearSelection();

    // ---- tools, colours, history ----
    void setTool(SnapTool tool);
    // An index outside the swatches is ignored.
    void setColor(int index);
    // While a label is being typed, undo throws it away and redo does nothing.
    void undo();
    void redo();
    void setBoxInfill(bool enabled);
    void setSnapEnabled(bool enabled);

Q_SIGNALS:
    void kept(const QImage& image);
    void dropped();
    // A selection started here; one snap covering several screens keeps only the newest one
    // (see clearSelection).
    void selecting();
    void snapToggled(bool enabled);
    void boxInfillToggled(bool enabled);

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void configure();
    void buildToolbar();
    void toggleBoxInfill();
    void toggleSnap();
    void afterHistory(const std::optional<Annotation>& item);
    void syncToolbar();
    bool shortcut(QKeyEvent* event);
    void press(QMouseEvent* event);
    void updateCursor(QPoint point);

    bool maySelect(QPoint point) const;
    void startSelection(QPoint point);
    void dragSelection(QPoint point);
    void finishSelection();
    std::optional<Grab> grabUnder(QPoint point) const;
    void startResize(Grab grab, QPoint point);
    void dragResize(QPoint point);
    void finishResize();
    void enterAnnotate();
    QPoint clamp(QPoint point) const;
    QPoint selectionPoint(QPoint point) const;
    void setSelection(std::optional<QRect> rect);
    QRegion decorRegion() const;

    void startAnnotation(QPointF point);
    void extendCurrentDraft(QPointF point, Qt::KeyboardModifiers modifiers);
    QPointF annotationPoint(QPointF point) const;
    void finishDraft();
    void add(Annotation item);
    QPointF inside(QPointF point) const;
    void updateMarks(const QRectF& rect);

    void openLabel(QPointF point);
    void commitLabel();
    void closeLabel();

    void paintSelection(QPainter& painter);
    void paintAnnotations(QPainter& painter) const;
    QString badgeText() const;
    QRect currentBadgeRect() const;
    bool toolbarShown() const;
    bool blocksHint(const QRect& rect) const;

    QImage frozen_;
    QImage dimmed_;
    QRect canvas_;
    QPointer<QScreen> screen_;
    SettingsStore* settings_ = nullptr;
    bool snapEnabled_ = false;
    QList<QRect> targets_;
    ui::Palette palette_;
    HintPill hint_;
    QFont badgeFont_;
    SnapPhase phase_ = SnapPhase::Select;
    std::optional<QRect> selection_;
    std::optional<QPoint> origin_;
    std::optional<Grab> grab_;
    QRect grabbedFrom_;
    QPoint grabShift_;
    AnnotationStack stack_;
    std::optional<Annotation> draft_;
    SnapTool tool_ = SnapTool::Arrow;
    int color_ = 0;
    bool boxInfill_ = false;
    QPointer<LabelEditor> editor_;
    bool done_ = false;
    SnapToolbar* toolbar_ = nullptr;
};

} // namespace fv::ui_snap
