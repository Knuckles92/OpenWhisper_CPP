#include "ui_hotkeys/hotkey_capture.h"

#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>

#include "appkit/qt_bridge.h"
#include "hotkeys/grammar.h"
#include "hotkeys/mouse_shortcuts.h"
#include "ui_theme/style_props.h"

namespace fv {

std::set<std::string> qtModifierNames(Qt::KeyboardModifiers flags, std::string_view platform) {
    const bool mac = platform == "darwin";
    std::set<std::string> names;
    if (flags & Qt::ControlModifier) {
        names.insert(mac ? "cmd" : "ctrl");
    }
    if (flags & Qt::MetaModifier) {
        names.insert(mac ? "ctrl" : platform == "win32" ? "win" : "cmd");
    }
    if (flags & Qt::AltModifier) {
        names.insert("alt");
    }
    if (flags & Qt::ShiftModifier) {
        names.insert("shift");
    }
    return names;
}

std::optional<std::string> sideButtonHotkey(Qt::MouseButton button,
                                            Qt::KeyboardModifiers modifiers) {
    std::string name;
    if (button == Qt::BackButton) {
        name = "mouse4";
    } else if (button == Qt::ForwardButton) {
        name = "mouse5";
    } else {
        return std::nullopt;
    }
    return hotkeys::format_hotkey(qtModifierNames(modifiers), name, hotkeys::platform_vocabulary());
}

std::optional<std::string> sideButtonHotkey(const QMouseEvent& event) {
    return sideButtonHotkey(event.button(), event.modifiers());
}

HotkeyCaptureInput::HotkeyCaptureInput(QWidget* parent) : QLineEdit(parent) {
    setObjectName(QStringLiteral("hotkeyInput"));
    setReadOnly(true);
    setMinimumHeight(38);
    setPlaceholderText(QStringLiteral("Click to set hotkey"));
}

void HotkeyCaptureInput::setCapturing(bool capturing) {
    captureActive_ = capturing;
    ui::setStyleProperty(this, ui::prop::capturing, capturing);
}

void HotkeyCaptureInput::setMouseShortcutsSupported(std::optional<bool> supported) {
    mouseShortcuts_ = supported;
}

bool HotkeyCaptureInput::mouseShortcutsSupported() const {
    return mouseShortcuts_.value_or(hotkeys::mouse_shortcuts_supported());
}

bool HotkeyCaptureInput::event(QEvent* event) {
    // Swallow Esc (and other shortcuts) while capturing, so a dialog does not take Escape as
    // reject and the key never becomes a global hotkey.
    if (captureActive_ && event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
    }
    return QLineEdit::event(event);
}

void HotkeyCaptureInput::keyPressEvent(QKeyEvent* event) {
    if (captureActive_ && event->key() == Qt::Key_Escape) {
        event->accept();
        // Ctrl+Alt+Esc and the like are left to the capture to bind.
        if ((event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
            Q_EMIT captureCancelled();
        }
        return;
    }
    QLineEdit::keyPressEvent(event);
}

void HotkeyCaptureInput::mousePressEvent(QMouseEvent* event) {
    const auto hotkey = sideButtonHotkey(*event);
    if (!hotkey) {
        Q_EMIT captureRequested();
        QLineEdit::mousePressEvent(event);
        return;
    }
    event->accept();
    if (!captureActive_) {
        return;
    }
    if (mouseShortcutsSupported()) {
        sideButtonCaptured(qt::toQString(*hotkey));
    } else {
        Q_EMIT notice(QString::fromUtf16(kMouseUnsupportedMessage));
    }
}

void HotkeyCaptureInput::sideButtonCaptured(const QString& hotkey) {
    Q_EMIT mouseCaptured(hotkey);
}

} // namespace fv
