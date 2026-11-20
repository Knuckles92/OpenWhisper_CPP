#pragma once

#include <optional>
#include <string_view>
#include <utility>

#include <QString>

#include "ui_hotkeys/hotkey_capture.h"

class QTimer;

namespace fv {

// A shortcut field that captures in Qt itself while the app's hotkeys are suspended
// (ui_qt/widgets/profile_hotkey_input.py). Click, Space or Enter starts a capture; the shortcut
// is committed on the release of its main key, so only the last key pressed counts; Escape with
// any modifiers, losing focus, hiding and 10 s without a shortcut cancel it. Auto-repeat is
// ignored. A Back or Forward click while capturing is a shortcut with the modifiers held.
// Affinity: GUI thread.
class ProfileHotkeyInput : public HotkeyCaptureInput {
    Q_OBJECT

public:
    // The capture gives up after this long without a shortcut.
    static constexpr int kCaptureTimeoutMs = 10000;

    explicit ProfileHotkeyInput(QWidget* parent = nullptr);

    // The saved shortcut ("" for none); setHotkey shows it as format_hotkey_display does.
    const QString& hotkey() const { return hotkey_; }
    void setHotkey(const QString& hotkey);

    void beginCapture();
    void cancelCapture();
    bool isCapturing() const { return capturing_; }

    // The key name the field gives a Qt key ("a", "f5", "page up"), or "" for a key it ignores;
    // `platform` as sys.platform names it ("page up" on Windows, "page_up" elsewhere).
    static QString keyName(int key, std::string_view platform);

Q_SIGNALS:
    void captured(const QString& hotkey);
    void captureChanged(bool capturing);

protected:
    bool event(QEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void sideButtonCaptured(const QString& hotkey) override;

private:
    QString hotkey_;
    bool capturing_ = false;
    // The main key pressed while capturing and the shortcut it makes, committed on its release.
    std::optional<std::pair<int, QString>> pending_;
    QTimer* timer_;
};

} // namespace fv
