#pragma once

#include <optional>

#include <QPointer>
#include <QPushButton>
#include <QRect>
#include <QString>
#include <QTimer>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QPropertyAnimation;

namespace fv {

// Floating pill-shaped shortcut hint shown above a button on hover. Affinity: GUI thread.
class HotkeyHoverHint : public QWidget {
    Q_OBJECT
public:
    explicit HotkeyHoverHint(QWidget* parent = nullptr);

    void setHotkey(const QString& hotkey);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    QLabel* textLabel_;
    QPropertyAnimation* fadeIn_;
};

// Shows a HotkeyHoverHint pill above a button while it is hovered. Attachable to any
// QPushButton without changing the button's font or sizing the way subclassing Button would; it
// parents itself to the button. Affinity: GUI thread.
class HotkeyHintFilter : public QObject {
    Q_OBJECT
public:
    static constexpr int kHideDelayMs = 80;

    // hotkey is the shortcut's display text; empty disables the hint.
    explicit HotkeyHintFilter(QPushButton* button, const QString& hotkey = {});

    void setHotkey(const QString& hotkey);
    // The pill, once one has been shown.
    HotkeyHoverHint* hint() const { return hint_; }

    void showHint();
    void hideHint();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    QPushButton* button_;
    QString hotkey_;
    QPointer<HotkeyHoverHint> hint_;
    QTimer* hideTimer_;
};

// Shows hotkey above area (in anchor's coordinates). Returns the pill to keep for next time,
// made on first use.
HotkeyHoverHint* showHotkeyHint(HotkeyHoverHint* hint, QWidget* anchor, const QRect& area,
                                const QString& hotkey);
void hideHotkeyHint(HotkeyHoverHint* hint);

// Modern button. Affinity: GUI thread.
class Button : public QPushButton {
    Q_OBJECT
public:
    explicit Button(const QString& text = {}, QWidget* parent = nullptr);

    // Re-fits the width to the label; call through Button, not QPushButton.
    void setText(const QString& text);
    void setHotkey(const QString& hotkey);

    // The minimum size used when the text changes.
    void setBaseMinimumSize(int width, int height);

    // Toggles interactivity while keeping the button hover-responsive. Unlike setEnabled(false)
    // the button stays enabled, so it still gets hover events (and shows its hotkey hint);
    // clicks, Space, Enter and Return are swallowed and the pointer is the arrow. For buttons
    // that always advertise their shortcut even when the action is not available.
    void setActive(bool active);
    bool isActive() const { return active_; }

    HotkeyHintFilter* hotkeyHintFilter() const { return hotkeyHintFilter_; }

protected:
    // The colour buttons' 48 x 140 floor.
    void useToneSize();

    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    // Sizes the button to fit its label without horizontal clipping. Polishes first so the font
    // metrics match the theme (14 px, weight 600) and not the constructor's fallback font.
    void refreshSize();

    bool active_ = true;
    int baseMinHeight_ = 44;
    int baseMinWidth_ = 140;
    HotkeyHintFilter* hotkeyHintFilter_;
};

// A compact dialog-chrome button: at least 34 px tall, never smaller than its polished label.
// width: the minimum width, the button fixed to that or its label's width, whichever is larger;
// 0 lets it stretch freely; nullopt leaves the width alone.
void fitCompactButton(Button* button, std::optional<int> width = std::nullopt);

// Outlined, compact styling for a secondary action on a Settings page. Driven by the tone
// property, not an object name, so a button keeps its own object name.
Button* neutralButton(Button* button);
// The accent fill of PrimaryButton at the compact size Settings uses, with any object name.
Button* compactPrimaryButton(Button* button);
// The danger fill at the compact size, for a destructive dialog action.
Button* compactDangerButton(Button* button);

// Width of the Start button that leads Quick Record, Walkthrough and Meeting Mode, so the same
// action looks the same on every tab.
inline constexpr int kStartButtonWidth = 340;

// Gives widget the Start button's width, narrower only in a narrower column. Add it with
// stretch 1 between two plain stretches.
QWidget* fitStartWidth(QWidget* widget);

// The theme styles these by object name (primaryButton, ...). Give an instance an identifier
// with the buttonId property, not setObjectName, or it loses its colours.
class PrimaryButton : public Button {
    Q_OBJECT
public:
    explicit PrimaryButton(const QString& text = {}, QWidget* parent = nullptr);
};

class DangerButton : public Button {
    Q_OBJECT
public:
    explicit DangerButton(const QString& text = {}, QWidget* parent = nullptr);
};

class SuccessButton : public Button {
    Q_OBJECT
public:
    explicit SuccessButton(const QString& text = {}, QWidget* parent = nullptr);
};

class WarningButton : public Button {
    Q_OBJECT
public:
    explicit WarningButton(const QString& text = {}, QWidget* parent = nullptr);
};

} // namespace fv
