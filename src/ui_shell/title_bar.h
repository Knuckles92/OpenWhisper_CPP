#pragma once

#include <optional>

#include <QFrame>
#include <QPoint>
#include <QPushButton>
#include <QRect>
#include <QString>

class QHBoxLayout;
class QLabel;
class QMenuBar;

namespace fv {

// Which window edges a point is near: -1 left or top, 0 neither, 1 right or bottom.
struct ResizeEdge {
    int horizontal = 0;
    int vertical = 0;
    bool none() const { return horizontal == 0 && vertical == 0; }
    friend bool operator==(const ResizeEdge&, const ResizeEdge&) = default;
};

// What the title bar asks of the frameless window it moves and resizes (MainWindow's
// _get_resize_edge, _begin_resize, _apply_resize_delta, _finish_resize,
// _leave_maximized_in_place and _compact_mode). Affinity: GUI thread.
class FramelessWindowHost {
public:
    virtual ~FramelessWindowHost() = default;
    virtual ResizeEdge resizeEdge(QPoint local) const = 0;
    virtual void beginResize(ResizeEdge edge, QPoint global) = 0;
    virtual void applyResizeDelta(QPoint global) = 0;
    virtual void finishResize() = 0;
    virtual bool isResizing() const = 0;
    virtual void leaveMaximizedInPlace() = 0;
    virtual bool isCompactMode() const = 0;
};

// A title-bar button that paints its glyph instead of drawing a character: text glyphs come
// from whatever font the platform falls back to, so they render at different sizes and
// weights, and "□" can turn into "=". Painted lines look the same everywhere.
// Affinity: GUI thread.
class WindowGlyphButton : public QPushButton {
    Q_OBJECT
public:
    enum class Glyph { Minimize, Maximize, Restore, Close };

    // hoverToken: the palette token the glyph takes while hovered.
    explicit WindowGlyphButton(Glyph glyph, const char* hoverToken = "text-heading",
                               QWidget* parent = nullptr);

    Glyph glyph() const { return glyph_; }
    // "minimize", "maximize", "restore" or "close", as Python's glyph property.
    QString glyphName() const;
    void setGlyph(Glyph glyph);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    Glyph glyph_;
    const char* hoverToken_;
};

// The frameless main window's title bar (object name "customTitleBar"): the menu bar, the
// centred product name, room for a notice, and the painted minimize, maximize and close
// buttons. Its height follows the menu bar, so a larger UI font never hides menu items behind
// QMenuBar's overflow button. Dragging it moves the window, pressing near an edge resizes it,
// and a double-click toggles maximize. Affinity: GUI thread.
class TitleBar : public QFrame {
    Q_OBJECT
public:
    // Height at the default font scale.
    static constexpr int kBaseHeight = 32;
    static constexpr int kButtonWidth = 46;

    // window: what the buttons minimize, maximize and close (null: a bar on its own); host: how
    // it resizes and moves (null: neither).
    explicit TitleBar(QWidget* window = nullptr, FramelessWindowHost* host = nullptr);

    QMenuBar* menuBar() const { return menuBar_; }
    QLabel* titleLabel() const { return titleLabel_; }
    WindowGlyphButton* minimizeButton() const { return minimizeButton_; }
    WindowGlyphButton* maximizeButton() const { return maximizeButton_; }
    WindowGlyphButton* closeButton() const { return closeButton_; }

    // Places widget just before the window buttons.
    void addNotice(QWidget* widget);
    // Follows a maximized state that changed without the maximize button.
    void syncMaximized(bool maximized);
    bool isMaximized() const { return maximized_; }
    // The maximize button: maximize, or restore the geometry from before. Ignored in Compact
    // Mode.
    void toggleMaximize();

protected:
    bool event(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void syncHeight();
    void showMaximizeGlyph(bool maximized);

    QWidget* window_;
    FramelessWindowHost* host_;
    QHBoxLayout* layout_;
    QMenuBar* menuBar_;
    QLabel* titleLabel_;
    WindowGlyphButton* minimizeButton_;
    WindowGlyphButton* maximizeButton_;
    WindowGlyphButton* closeButton_;
    std::optional<QPoint> dragPosition_;
    bool maximized_ = false;
    std::optional<QRect> normalGeometry_;
};

} // namespace fv
