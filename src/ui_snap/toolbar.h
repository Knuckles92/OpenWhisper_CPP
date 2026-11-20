#pragma once

#include <optional>
#include <vector>

#include <QColor>
#include <QFont>
#include <QMap>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QWidget>

#include "ui_snap/annotations.h"
#include "ui_theme/palette.h"

class QFontMetrics;
class QPainter;
class QRectF;

// The compact annotate toolbar that rides under the selection (ui_qt/snap/toolbar.py). One
// painted widget rather than a row of tool buttons: the app stylesheet restyles every
// QToolButton, and here each control needs its own look (tool icons tinted with the chosen
// colour, round swatches, a filled Keep). It never takes keyboard focus, so typing and shortcuts
// stay with the overlay. The widget is exactly the panel; the overlay paints its shadow, so no
// margin around it swallows clicks meant for the screenshot.
namespace fv::ui_snap {

inline constexpr int kToolbarButton = 34;
inline constexpr int kToolbarSwatch = 26;
inline constexpr int kToolbarIcon = 20;
inline constexpr int kToolbarPad = 6;
inline constexpr int kToolbarSeparator = 13;
inline constexpr double kToolbarRadius = 12.0;
inline constexpr int kToolbarLabelPad = 6;
inline constexpr int kToolbarLabelGap = 4;

// One control: kind is "tool", "undo", "redo", "color", "infill", "snap", "region", "delay",
// "keep_marks", "clear", "drop" or "keep"; value is a tool's name, a swatch's index, else the
// kind. label is empty for an icon-only button.
struct ToolbarButton {
    QString kind;
    QString value;
    QRect rect;
    QString tip;
    QString label;
    friend bool operator==(const ToolbarButton&, const ToolbarButton&) = default;
};

// Buttons left to right and the (x, top) of each group separator, inside the panel.
struct ToolbarLayout {
    std::vector<ToolbarButton> buttons;
    std::vector<QPoint> separators;
};

// The shortcuts the live toolbar names in its Keep marks and Clear marks tips, keyed
// "keep_marks" and "clear"; a missing key reads "K while drawing" or "C while drawing".
using MarkShortcuts = QMap<QString, QString>;

// _layout: every control in one row, measured with the labels' font.
ToolbarLayout toolbarLayout(bool live, const QFontMetrics& labelMetrics,
                            const MarkShortcuts& markShortcuts = {});
// _wrap: whole control groups moved to another row when the panel is wider than width.
ToolbarLayout wrapToolbar(const std::vector<ToolbarButton>& buttons, int width);

// Tools, undo and redo, colours, then Cancel and Keep; the live variant (M6.10's live drawing)
// has no undo, redo or Keep, and adds Region, the capture delay chip's slot, Keep marks and
// Clear marks. Affinity: GUI thread.
class SnapToolbar : public QWidget {
    Q_OBJECT

public:
    enum class Variant { Overlay, Live };

    SnapToolbar(QWidget* parent, const ui::Palette& palette, Variant variant = Variant::Overlay,
                Qt::WindowFlags flags = {}, MarkShortcuts markShortcuts = {});

    Variant variant() const { return variant_; }
    // Lays the controls out again, wrapped when the panel would be wider than width.
    void fitToWidth(int width);
    // nullopt tool: none chosen (the live toolbar between marks).
    void setState(std::optional<SnapTool> tool, int color, bool canUndo, bool canRedo);
    void setSnapEnabled(bool enabled);
    void setBoxInfill(bool enabled);
    void setKeepMarks(bool enabled);
    void setRegionActive(bool active);
    // Whether × has anything to put down or cancel (the live toolbar only).
    void setDropEnabled(bool enabled);

    std::optional<SnapTool> tool() const { return tool_; }
    int colorIndex() const { return color_; }
    bool canUndo() const { return canUndo_; }
    bool canRedo() const { return canRedo_; }
    bool snapEnabled() const { return snapEnabled_; }
    bool boxInfill() const { return boxInfill_; }
    bool keepMarks() const { return keepMarks_; }
    bool regionActive() const { return regionActive_; }
    bool dropEnabled() const { return dropEnabled_; }
    const std::vector<ToolbarButton>& buttons() const { return layout_.buttons; }
    const std::vector<QPoint>& separators() const { return layout_.separators; }

    // Where a control is, for tests and for anchoring popups; empty if unknown. An empty value
    // matches the kind's first control.
    QRect buttonRect(const QString& kind, const QString& value = QString()) const;
    QRect buttonRect(SnapTool tool) const;
    // The tooltip a control shows now: its own, or why it is disabled.
    QString tipFor(const ToolbarButton& button) const;

    // The live toolbar's capture delay chip, which paints itself and owns its preset picker: it
    // becomes this toolbar's child and sits on the "delay" slot, also after fitToWidth. The
    // overlay variant has no slot and ignores it.
    void setCaptureDelay(QWidget* chip);
    QWidget* captureDelay() const { return captureDelay_; }

Q_SIGNALS:
    void toolChosen(fv::ui_snap::SnapTool tool);
    void colorChosen(int index);
    void undoClicked();
    void redoClicked();
    void dropClicked();
    void keepClicked();
    void snapClicked();
    void infillClicked();
    void keepMarksClicked();
    void clearClicked();
    void regionClicked();

protected:
    bool event(QEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    bool enabled(const ToolbarButton& button) const;
    // The index of the control at pos, or -1.
    int buttonAt(QPoint pos, bool enabledOnly = true) const;
    void activate(const ToolbarButton& button);
    void placeCaptureDelay();
    void paintSeparators(QPainter& painter) const;
    void paintButton(QPainter& painter, int index) const;
    void paintLabeledButton(QPainter& painter, const ToolbarButton& button, const QRectF& rect,
                            const QColor& icon) const;
    // The fill behind a button (nullopt for none) and its icon colour.
    std::pair<std::optional<QColor>, QColor> buttonColors(int index) const;
    void paintSwatch(QPainter& painter, int index) const;

    ui::Palette palette_;
    QFont labelFont_;
    Variant variant_;
    MarkShortcuts markShortcuts_;
    ToolbarLayout layout_;
    QPointer<QWidget> captureDelay_;
    std::optional<SnapTool> tool_ = SnapTool::Arrow;
    int color_ = 0;
    bool canUndo_ = false;
    bool canRedo_ = false;
    bool snapEnabled_ = false;
    bool boxInfill_ = false;
    bool keepMarks_ = false;
    bool regionActive_ = false;
    bool dropEnabled_ = true;
    int hover_ = -1;
    int pressed_ = -1;
};

// The tool a toolbar value names ("arrow", "highlighter"), as Python's Tool(value).
std::optional<SnapTool> toolFromName(const QString& name);

} // namespace fv::ui_snap
