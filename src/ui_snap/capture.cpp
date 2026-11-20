#include "ui_snap/capture.h"

#include <algorithm>
#include <cmath>
#include <exception>

#include <QCursor>
#include <QGuiApplication>
#include <QScreen>

#include "base/log.h"
#include "ui_snap/affinity.h"
#include "ui_snap/desktop.h"
#include "ui_snap/support.h"

namespace fv::ui_snap {
namespace {

constexpr log::Logger kLog{"ui_qt.snap.capture"};

bool paintable(QImage::Format format) {
    return format == QImage::Format_RGB32 || format == QImage::Format_ARGB32 ||
           format == QImage::Format_ARGB32_Premultiplied;
}

QList<QWidget*> topLevels(const QList<QPointer<QWidget>>& windows) {
    QList<QWidget*> found;
    for (const QPointer<QWidget>& widget : windows) {
        if (!widget) {
            continue;
        }
        QWidget* top = widget->window();
        if (!found.contains(top)) {
            found.append(top);
        }
    }
    return found;
}

} // namespace

QScreen* screenUnderCursor() {
    if (QScreen* screen = QGuiApplication::screenAt(QCursor::pos())) {
        return screen;
    }
    return QGuiApplication::primaryScreen();
}

QScreen* screenAtWindowsPoint(QPoint point) {
    std::optional<NativeRect> monitor;
    try {
        monitor = win32Desktop().monitorAt(point, false);
    } catch (const std::exception& error) {
        kLog.warning("Could not find the clicked monitor: {}", error.what());
        return nullptr;
    }
    if (!monitor) {
        return nullptr;
    }
    // Qt scales screen sizes on Windows but leaves their desktop origins intact.
    const QPoint origin(monitor->left, monitor->top);
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen->geometry().topLeft() == origin) {
            return screen;
        }
    }
    return nullptr;
}

QScreen* screenAtClick(QPoint point) {
    if (systemPlatform() == "win32") {
        return screenAtWindowsPoint(point);
    }
    return QGuiApplication::screenAt(point);
}

QList<QScreen*> screensToCover() {
    QScreen* pointed = screenUnderCursor();
    if (pointed == nullptr) {
        return {};
    }
    if (usesGrim()) {
        return {pointed};
    }
    QList<QScreen*> screens{pointed};
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen != pointed) {
            screens.append(screen);
        }
    }
    return screens;
}

std::optional<QImage> grab(QScreen* screen) {
    std::optional<QImage> image;
    try {
        image = screenGrabber().grabScreen(*screen);
    } catch (const std::exception& error) {
        kLog.warning("Capturing the screen for a snap failed: {}", error.what());
        return std::nullopt;
    }
    if (!image || image->isNull()) {
        return std::nullopt;
    }
    if (!paintable(image->format())) {
        image = image->convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    const int width = screen->geometry().width();
    image->setDevicePixelRatio(width > 0 ? static_cast<double>(image->width()) / width
                                         : screen->devicePixelRatio());
    return image;
}

int settleMs(double refreshRate, bool grim, bool cocoa) {
    const int twoFrames =
        refreshRate >= 24 ? static_cast<int>(std::ceil(2000 / refreshRate)) : kMinSettleMs;
    int floor = grim ? kHyprlandSettleMs : kMinSettleMs;
    if (cocoa) {
        floor = std::max(floor, kMacosSettleMs);
    }
    return std::max(twoFrames, floor);
}

int settleMs(QScreen* screen) {
    const double rate = screen != nullptr ? screen->refreshRate() : 0.0;
    return settleMs(rate, usesGrim(), qtPlatformName() == QStringLiteral("cocoa"));
}

QList<QPointer<QWidget>> hideWindows(const QList<QPointer<QWidget>>& windows) {
    QList<QPointer<QWidget>> hidden;
    for (QWidget* window : topLevels(windows)) {
        if (window->isVisible() && !window->isMinimized() && !isKeptOut(window)) {
            window->QWidget::hide();
            hidden.append(window);
        }
    }
    return hidden;
}

void restoreWindows(const QList<QPointer<QWidget>>& windows) {
    for (const QPointer<QWidget>& window : windows) {
        if (!window) {
            continue;
        }
        const bool quiet = window->testAttribute(Qt::WA_ShowWithoutActivating);
        window->setAttribute(Qt::WA_ShowWithoutActivating, true);
        window->QWidget::show();
        window->setAttribute(Qt::WA_ShowWithoutActivating, quiet);
    }
}

} // namespace fv::ui_snap
