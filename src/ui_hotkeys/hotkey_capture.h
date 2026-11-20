#pragma once

#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <QLineEdit>
#include <QString>

#include "base/platform_info.h"

class QMouseEvent;

// The read-only shortcut field of Settings and its helpers (ui_qt/widgets/hotkey_capture.py).
// The global capture itself is hotkeys::HotkeyCapture.
namespace fv {

// CAPTURE_HINT: shown while a shortcut field is waiting for a key. Escape backs out.
inline constexpr char16_t kCaptureHint[] = u"Press keys… (Esc cancels)";
// MOUSE_UNSUPPORTED_MESSAGE.
inline constexpr char16_t kMouseUnsupportedMessage[] =
    u"Mouse buttons aren't supported as shortcuts on this desktop yet";

// qt_modifier_names: Qt keyboard modifiers as the backend's modifier names. Control is "cmd" on
// macOS, else "ctrl"; Meta is "ctrl" on macOS, "win" on Windows, "cmd" elsewhere; Alt "alt",
// Shift "shift". `platform` is named as sys.platform names it.
std::set<std::string> qtModifierNames(Qt::KeyboardModifiers flags,
                                      std::string_view platform = python_platform());

// side_button_hotkey: the shortcut a Back ("mouse4") or Forward ("mouse5") click spells, with
// its modifiers; nullopt for any other button.
std::optional<std::string> sideButtonHotkey(Qt::MouseButton button,
                                            Qt::KeyboardModifiers modifiers);
std::optional<std::string> sideButtonHotkey(const QMouseEvent& event);

// Read-only shortcut field (#hotkeyInput) that requests a capture when clicked. While
// capturing, a Back or Forward click on the field is a shortcut too: mouseCaptured carries it,
// or notice explains that this desktop cannot use mouse buttons; outside a capture such a click
// does nothing.
// Affinity: GUI thread.
class HotkeyCaptureInput : public QLineEdit {
    Q_OBJECT

public:
    explicit HotkeyCaptureInput(QWidget* parent = nullptr);

    // set_capturing: also the `capturing` style property.
    void setCapturing(bool capturing);
    bool isCaptureActive() const { return captureActive_; }

    // Whether a side button can be a shortcut: hotkeys::mouse_shortcuts_supported() unless set
    // here (tests stand in for a desktop without them).
    void setMouseShortcutsSupported(std::optional<bool> supported);
    bool mouseShortcutsSupported() const;

Q_SIGNALS:
    void captureRequested();
    void captureCancelled();
    void mouseCaptured(const QString& hotkey);
    void notice(const QString& text);

protected:
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    // A side button was clicked while capturing on a desktop that supports it.
    virtual void sideButtonCaptured(const QString& hotkey);

private:
    bool captureActive_ = false;
    std::optional<bool> mouseShortcuts_;
};

} // namespace fv
