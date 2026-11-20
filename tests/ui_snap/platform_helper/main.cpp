// The other process of test_interactive.cpp: Snap's real Windows plumbing on Qt's windows
// platform, which the offscreen test executable cannot load. It shows a small magenta window,
// keeps it out of captures with SetWindowDisplayAffinity, grabs the real screen with
// grabWindow(0), reads the real window list and forces the foreground. Exit code 0 when every
// check held; each failure is printed.
#include <cstdio>
#include <string>

#include <QApplication>
#include <QColor>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QPalette>
#include <QScreen>
#include <QThread>
#include <QWidget>

#include "ui_snap/affinity.h"
#include "ui_snap/capture.h"
#include "ui_snap/desktop.h"
#include "ui_snap/pointed.h"

namespace {

int failures = 0;

void check(bool held, const char* what) {
    std::printf("%s: %s\n", held ? "ok" : "FAILED", what);
    failures += held ? 0 : 1;
}

void settle(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
}

bool magentaAt(const QImage& image, QPoint logical) {
    const double dpr = image.devicePixelRatio();
    const QPoint device(static_cast<int>(logical.x() * dpr), static_cast<int>(logical.y() * dpr));
    const QColor color = image.pixelColor(device);
    return color.red() > 220 && color.green() < 40 && color.blue() > 220;
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "windows");
    QApplication app(argc, argv);
    using namespace fv::ui_snap;

    QScreen* screen = QGuiApplication::primaryScreen();
    QWidget window;
    window.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    QPalette palette = window.palette();
    palette.setColor(QPalette::Window, QColor(255, 0, 255));
    window.setPalette(palette);
    window.setAutoFillBackground(true);
    const QRect area = screen->geometry();
    window.setGeometry(QRect(area.topLeft() + QPoint(40, 40), QSize(160, 120)));
    window.show();
    settle(settleMs(screen) * 4);
    const QPoint inside = QPoint(40, 40) + QPoint(80, 60);

    const std::optional<QImage> before = grab(screen);
    check(before.has_value(), "grabWindow(0) captures the screen");
    if (before) {
        check(before->deviceIndependentSize().toSize() == area.size(),
              "the capture's ratio maps it onto the screen's logical size");
        check(magentaAt(*before, inside), "a shown window is in the capture");
    }

    check(keepOutOfScreenshots(&window, true), "SetWindowDisplayAffinity excludes the window");
    check(isKeptOut(&window), "the exclusion is remembered");
    check(realWin32Desktop().windowDisplayAffinity(window.winId()) == kWdaExcludeFromCapture,
          "GetWindowDisplayAffinity reads WDA_EXCLUDEFROMCAPTURE");
    settle(settleMs(screen) * 4);
    const std::optional<QImage> excluded = grab(screen);
    check(excluded && !magentaAt(*excluded, inside), "an excluded window is not captured");
    check(window.isVisible(), "an excluded window stays on screen");

    const QList<QRect> targets = windowTargets(screen);
    check(!targets.isEmpty(), "EnumWindows finds window edges");
    const QRect frame(QPoint(40, 40), QSize(160, 120));
    check(targets.contains(frame), "our window's frame is a target when it is not ours");
    check(!windowTargets(screen, {&window}).contains(frame), "our own window is no target");

    bringToFront(&window);
    settle(100);
    check(realWin32Desktop().foregroundWindow() == window.winId(),
          "AttachThreadInput and SetForegroundWindow bring the window to the front");
    check(windowUnderPointer(screen, {}, true) == frame,
          "the window in front is the foreground window, in logical pixels");

    check(keepOutOfScreenshots(&window, false), "the exclusion is lifted");
    check(realWin32Desktop().windowDisplayAffinity(window.winId()) == kWdaNone,
          "GetWindowDisplayAffinity reads WDA_NONE again");
    window.close();
    std::printf("%d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
