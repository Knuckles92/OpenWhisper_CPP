#include "ui_hotkeys/profile_hotkey_input.h"

#include <QEvent>
#include <QKeyEvent>
#include <QTimer>

#include "appkit/qt_bridge.h"
#include "base/platform_info.h"
#include "hotkeys/grammar.h"

namespace fv {

ProfileHotkeyInput::ProfileHotkeyInput(QWidget* parent)
    : HotkeyCaptureInput(parent), timer_(new QTimer(this)) {
    setAccessibleName(QStringLiteral("Profile recording shortcut"));
    setToolTip(QStringLiteral(
        "Click, then press a shortcut. Escape cancels. Profile shortcuts toggle recording."));
    connect(this, &HotkeyCaptureInput::captureRequested, this, &ProfileHotkeyInput::beginCapture);
    timer_->setSingleShot(true);
    timer_->setInterval(kCaptureTimeoutMs);
    connect(timer_, &QTimer::timeout, this, &ProfileHotkeyInput::cancelCapture);
}

void ProfileHotkeyInput::setHotkey(const QString& hotkey) {
    hotkey_ = hotkey;
    setText(qt::toQString(
        hotkeys::format_hotkey_display(qt::toUtf8(hotkey), hotkeys::platform_vocabulary())));
}

void ProfileHotkeyInput::beginCapture() {
    if (capturing_) {
        return;
    }
    capturing_ = true;
    pending_.reset();
    setCapturing(true);
    setText(QString::fromUtf16(kCaptureHint));
    Q_EMIT captureChanged(true);
    timer_->start();
}

void ProfileHotkeyInput::cancelCapture() {
    if (!capturing_) {
        return;
    }
    capturing_ = false;
    pending_.reset();
    timer_->stop();
    setCapturing(false);
    setHotkey(hotkey_);
    Q_EMIT captureChanged(false);
}

void ProfileHotkeyInput::focusOutEvent(QFocusEvent* event) {
    cancelCapture();
    HotkeyCaptureInput::focusOutEvent(event);
}

void ProfileHotkeyInput::hideEvent(QHideEvent* event) {
    cancelCapture();
    HotkeyCaptureInput::hideEvent(event);
}

bool ProfileHotkeyInput::event(QEvent* event) {
    if (capturing_) {
        if (event->type() == QEvent::ShortcutOverride) {
            event->accept();
            return true;
        }
        // Tab and Backtab are shortcuts here, not focus moves.
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
                keyPressEvent(key);
                return true;
            }
        }
    }
    return HotkeyCaptureInput::event(event);
}

QString ProfileHotkeyInput::keyName(int key, std::string_view platform) {
    const bool windows = platform == "win32";
    QString name;
    switch (key) {
    case Qt::Key_Space:
        name = QStringLiteral("space");
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        name = QStringLiteral("enter");
        break;
    case Qt::Key_Backspace:
        name = QStringLiteral("backspace");
        break;
    case Qt::Key_Delete:
        name = QStringLiteral("delete");
        break;
    case Qt::Key_Insert:
        name = QStringLiteral("insert");
        break;
    case Qt::Key_Home:
        name = QStringLiteral("home");
        break;
    case Qt::Key_End:
        name = QStringLiteral("end");
        break;
    case Qt::Key_PageUp:
        name = windows ? QStringLiteral("page up") : QStringLiteral("page_up");
        break;
    case Qt::Key_PageDown:
        name = windows ? QStringLiteral("page down") : QStringLiteral("page_down");
        break;
    case Qt::Key_Left:
        name = QStringLiteral("left");
        break;
    case Qt::Key_Right:
        name = QStringLiteral("right");
        break;
    case Qt::Key_Up:
        name = QStringLiteral("up");
        break;
    case Qt::Key_Down:
        name = QStringLiteral("down");
        break;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        name = QStringLiteral("tab");
        break;
    default:
        break;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        name = QStringLiteral("f%1").arg(key - Qt::Key_F1 + 1);
    } else if (key >= 33 && key <= 126 && key != Qt::Key_Plus) {
        // Qt's key codes of printable ASCII are the upper-case characters.
        name = QString(QChar(key)).toLower();
    }
    return name;
}

void ProfileHotkeyInput::keyPressEvent(QKeyEvent* event) {
    const int key = event->key();
    if (!capturing_) {
        if (key == Qt::Key_Space || key == Qt::Key_Return || key == Qt::Key_Enter) {
            beginCapture();
            event->accept();
            return;
        }
        HotkeyCaptureInput::keyPressEvent(event);
        return;
    }
    event->accept();
    if (event->isAutoRepeat()) {
        return;
    }
    if (key == Qt::Key_Escape) {
        cancelCapture();
        return;
    }
    const Qt::KeyboardModifiers flags = event->modifiers();
    const std::string_view platform = python_platform();
    QString name = keyName(key, platform);
    if (name.isEmpty()) {
        return;
    }
    if (platform != "darwin" && (flags & Qt::KeypadModifier)) {
        name = QStringLiteral("kp ") + name;
    }
    pending_.emplace(key, qt::toQString(hotkeys::format_hotkey(qtModifierNames(flags, platform),
                                                               qt::toUtf8(name),
                                                               hotkeys::platform_vocabulary())));
}

void ProfileHotkeyInput::sideButtonCaptured(const QString& hotkey) {
    setHotkey(hotkey);
    cancelCapture();
    Q_EMIT captured(hotkey);
}

void ProfileHotkeyInput::keyReleaseEvent(QKeyEvent* event) {
    if (capturing_ && pending_ && !event->isAutoRepeat()) {
        const auto [key, hotkey] = *pending_;
        if (event->key() == key) {
            setHotkey(hotkey);
            cancelCapture();
            Q_EMIT captured(hotkey);
            event->accept();
            return;
        }
    }
    HotkeyCaptureInput::keyReleaseEvent(event);
}

} // namespace fv
