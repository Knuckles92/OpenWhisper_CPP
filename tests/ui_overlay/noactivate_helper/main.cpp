// The other process of "native window is non activating on windows": builds the overlay on Qt's
// "windows" platform, which the offscreen test executable cannot load, creates its native window
// without showing it, and prints the window's WS_EX_NOACTIVATE bit (0 when unset).
//
// Exit status 0 when the bit was read, 2 when the windows platform did not load.

#include <windows.h>

#include <cstdio>

#include <QApplication>

#include "ui_overlay/waveform_overlay.h"

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "windows");
    QApplication app(argc, argv);
    if (QGuiApplication::platformName() != QStringLiteral("windows")) {
        std::fprintf(stderr, "The windows platform plugin did not load.\n");
        return 2;
    }
    fv::WaveformOverlay overlay;
    const auto hwnd = reinterpret_cast<HWND>(overlay.winId());
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    std::printf("%lld\n", static_cast<long long>(style & WS_EX_NOACTIVATE));
    return 0;
}
