#include "ui_snap/pointed.h"

#include <exception>

#include <QCursor>
#include <QRegion>
#include <QScreen>
#include <QSet>

#include "base/log.h"
#include "pyport/number.h"

namespace fv::ui_snap {
namespace {

constexpr log::Logger kLog{"ui_qt.snap.pointed"};

// Frames smaller than this either way are not windows worth snapping to.
constexpr int kMinTarget = 24;

int pyRound(double value) {
    return static_cast<int>(py::round_half_even(value));
}

QRect win32Frame(WId window, QPoint origin, double scale) {
    const std::optional<NativeRect> frame = windowLister().frame(window);
    if (!frame) {
        return QRect(); // The window may have closed during enumeration.
    }
    return logicalFrame(*frame, origin, scale);
}

std::optional<QRect> win32Window(QScreen& screen, bool front) {
    WindowLister& lister = windowLister();
    Win32Desktop& desktop = win32Desktop();
    const QPoint point = lister.cursorPos();
    const WId window = front ? desktop.foregroundWindow() : lister.rootWindowAt(point);
    if (window == 0) {
        return std::nullopt;
    }
    std::optional<NativeRect> monitor;
    try {
        monitor = desktop.monitorAt(point, true);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!monitor) {
        return std::nullopt;
    }
    // Physical pixels from the monitor's corner, then the screen's logical ones.
    return win32Frame(window, QPoint(monitor->left, monitor->top), screen.devicePixelRatio());
}

} // namespace

QRect logicalFrame(const NativeRect& frame, QPoint origin, double scale) {
    return QRect(
        pyRound((frame.left - origin.x()) / scale), pyRound((frame.top - origin.y()) / scale),
        pyRound((frame.right - frame.left) / scale), pyRound((frame.bottom - frame.top) / scale));
}

bool pointedSupported() {
    const QString platform = qtPlatformName();
    return platform == QStringLiteral("windows") || platform == QStringLiteral("cocoa");
}

std::optional<QRect> windowUnderPointer(QScreen* screen, const QList<QPointer<QWidget>>& ours,
                                        bool front) {
    const QPoint pointer = QCursor::pos();
    for (const QPointer<QWidget>& widget : ours) {
        if (front) {
            break;
        }
        front =
            widget && widget->isVisible() && widget->window()->frameGeometry().contains(pointer);
    }
    std::optional<QRect> found;
    try {
        if (qtPlatformName() != QStringLiteral("windows")) {
            // Quartz (the cocoa platform) is not ported; other desktops have no answer.
            return std::nullopt;
        }
        found = win32Window(*screen, front);
    } catch (const std::exception& error) {
        kLog.warning("Could not find the window under the pointer: {}", error.what());
        return std::nullopt;
    }
    if (!found) {
        return std::nullopt;
    }
    const QRect area(QPoint(0, 0), screen->geometry().size());
    const QRect clipped = found->intersected(area);
    if (clipped.isEmpty()) {
        return std::nullopt;
    }
    return clipped;
}

QList<QRect> windowTargets(QScreen* screen, const QList<QPointer<QWidget>>& ours) {
    const QRect area(QPoint(), screen->geometry().size());
    QList<QRect> frames;
    try {
        if (qtPlatformName() != QStringLiteral("windows")) {
            // Quartz (the cocoa platform) is not ported; other desktops have no answer.
            return {};
        }
        const QPoint origin = screen->geometry().topLeft();
        QSet<WId> excluded;
        for (const QPointer<QWidget>& widget : ours) {
            if (widget && widget->isVisible()) {
                excluded.insert(widget->window()->winId());
            }
        }
        for (const ListedWindow& window : windowLister().windows()) {
            if (excluded.contains(window.handle) || !window.visible || window.iconic) {
                continue;
            }
            if (!window.cloaked.has_value() || !*window.cloaked) {
                frames.append(win32Frame(window.handle, origin, screen->devicePixelRatio()));
            }
        }
    } catch (const std::exception& error) {
        kLog.warning("Could not read window edges for snapping: {}", error.what());
        return {};
    }
    QRegion covered;
    QList<QRect> visible;
    for (const QRect& frame : frames) {
        if (frame.width() < kMinTarget || frame.height() < kMinTarget) {
            continue;
        }
        const QRegion onScreen(frame.intersected(area));
        if (!onScreen.subtracted(covered).isEmpty()) {
            // Keep actual frame edges when a window straddles two monitors.
            visible.append(frame);
            covered = covered.united(onScreen);
        }
    }
    return visible;
}

} // namespace fv::ui_snap
