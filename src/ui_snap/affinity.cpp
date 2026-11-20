#include "ui_snap/affinity.h"

#include <exception>

#include <QHash>
#include <QObject>
#include <QWindow>

#include "base/log.h"
#include "ui_snap/desktop.h"

namespace fv::ui_snap {
namespace {

constexpr log::Logger kLog{"ui_qt.snap.affinity"};

// The native handle the exclusion was applied to, per top-level widget. OpenWhisper keeps it
// in the window's dynamic property openwhisperSnapExcludedHwnd; widget code here sets no raw
// dynamic properties (lint.style_props), so it lives beside the widget and leaves with it.
QHash<const QWidget*, WId>& excludedHandles() {
    static QHash<const QWidget*, WId> handles;
    return handles;
}

void remember(QWidget* top, WId handle) {
    auto& handles = excludedHandles();
    if (!handles.contains(top)) {
        QObject::connect(top, &QObject::destroyed, [top] { excludedHandles().remove(top); });
    }
    handles.insert(top, handle);
}

void forceForeground(WId window) {
    Win32Desktop& desktop = win32Desktop();
    const WId foreground = desktop.foregroundWindow();
    if (foreground == window) {
        return;
    }
    const std::uint32_t theirs = foreground != 0 ? desktop.windowThreadProcessId(foreground) : 0;
    const std::uint32_t ours = desktop.currentThreadId();
    const bool attached =
        theirs != 0 && theirs != ours && desktop.attachThreadInput(theirs, ours, true);
    try {
        desktop.setForegroundWindow(window);
    } catch (...) {
        if (attached) {
            desktop.attachThreadInput(theirs, ours, false);
        }
        throw;
    }
    if (attached) {
        desktop.attachThreadInput(theirs, ours, false);
    }
}

} // namespace

bool keepOutOfScreenshots(QWidget* window, bool enabled) {
    if (systemPlatform() != "win32") {
        return false;
    }
    QWidget* top = window->window();
    const WId handle = top->winId();
    const std::uint32_t affinity = enabled ? kWdaExcludeFromCapture : kWdaNone;
    bool applied = false;
    try {
        applied = win32Desktop().setWindowDisplayAffinity(handle, affinity);
    } catch (const std::exception& error) {
        kLog.warning("Setting a window's capture affinity failed: {}", error.what());
        applied = false;
    }
    remember(top, enabled && applied ? handle : 0);
    if (!applied) {
        // Before Windows 10 2004 there is no exclusion: Snap hides the window instead.
        kLog.info("Windows did not change the window's capture affinity ({:#x})", affinity);
    }
    return applied;
}

bool isKeptOut(QWidget* window) {
    if (systemPlatform() != "win32") {
        return false;
    }
    QWidget* top = window->window();
    const WId recorded = excludedHandles().value(top, 0);
    if (recorded == 0 || top->windowHandle() == nullptr) {
        return false;
    }
    return top->winId() == recorded;
}

void bringToFront(QWidget* window) {
    if (qtPlatformName() != QStringLiteral("windows")) {
        return;
    }
    try {
        forceForeground(window->winId());
    } catch (const std::exception& error) {
        kLog.warning("Bringing the snap overlay to the front failed: {}", error.what());
    }
}

WId foregroundWindow() {
    if (qtPlatformName() == QStringLiteral("windows")) {
        return win32Desktop().foregroundWindow();
    }
    // The macOS application to return to (macos.foreground_app) is not ported.
    return 0;
}

void returnForeground(WId previous, const QList<QPointer<QWidget>>& windows) {
    if (qtPlatformName() == QStringLiteral("cocoa")) {
        // macOS returns the focus through its own window server (not ported).
        return;
    }
    const WId current = foregroundWindow();
    if (previous == 0 || current == 0) {
        return;
    }
    for (const QPointer<QWidget>& window : windows) {
        if (window && window->isVisible() && window->winId() == current) {
            forceForeground(previous);
            return;
        }
    }
}

} // namespace fv::ui_snap
