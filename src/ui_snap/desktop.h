#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QImage>
#include <QPoint>
#include <QString>
#include <qwindowdefs.h>

class QScreen;

// The seams between Snap and the desktop it captures. Each is set process-wide: the real one of
// this platform by default, a fake from ui_snap/testing/fake_desktop.h in tests, so no test grabs
// the owner's screen, reads their windows or moves their focus. Affinity: GUI thread, like the
// Qt calls they stand beside.
namespace fv::ui_snap {

// A screen's pixels at device resolution: QScreen::grabWindow(0). nullopt when the platform
// gave nothing; may throw, which grab() reports as a failed capture.
class ScreenGrabber {
public:
    virtual ~ScreenGrabber() = default;
    virtual std::optional<QImage> grabScreen(QScreen& screen) = 0;
};

// A Win32 RECT: physical pixels of the virtual desktop, right and bottom exclusive.
struct NativeRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    friend bool operator==(const NativeRect&, const NativeRect&) = default;
};

// The user32 calls of capture exclusion and foreground forcing, and MonitorFromPoint. Window
// handles are WIds (HWND values); thread ids are DWORDs.
class Win32Desktop {
public:
    virtual ~Win32Desktop() = default;
    // SetWindowDisplayAffinity: true when the system applied it.
    virtual bool setWindowDisplayAffinity(WId window, std::uint32_t affinity) = 0;
    // GetWindowDisplayAffinity, nullopt when the call fails.
    virtual std::optional<std::uint32_t> windowDisplayAffinity(WId window) = 0;
    virtual WId foregroundWindow() = 0;                          // 0 when none
    virtual std::uint32_t windowThreadProcessId(WId window) = 0; // its thread, 0 on failure
    virtual std::uint32_t currentThreadId() = 0;
    virtual bool attachThreadInput(std::uint32_t from, std::uint32_t to, bool attach) = 0;
    virtual bool setForegroundWindow(WId window) = 0;
    // The rcMonitor of MonitorFromPoint(point): with nearest, MONITOR_DEFAULTTONEAREST, else
    // MONITOR_DEFAULTTONULL, when nullopt means no monitor is there. Throws std::system_error
    // when the monitor's information cannot be read.
    virtual std::optional<NativeRect> monitorAt(QPoint point, bool nearest) = 0;
};

// One top-level window of EnumWindows.
struct ListedWindow {
    WId handle = 0;
    bool visible = false; // IsWindowVisible
    bool iconic = false;  // IsIconic
    // DWMWA_CLOAKED (14) is non-zero; nullopt when DwmGetWindowAttribute failed.
    std::optional<bool> cloaked;
};

// The window manager's view of the desktop, as pointed.py asks for it.
class WindowLister {
public:
    virtual ~WindowLister() = default;
    virtual QPoint cursorPos() = 0; // GetCursorPos, physical pixels
    // GetAncestor(WindowFromPoint(point), GA_ROOT); 0 when there is none.
    virtual WId rootWindowAt(QPoint point) = 0;
    // EnumWindows, front to back. May throw, which the caller logs.
    virtual std::vector<ListedWindow> windows() = 0;
    // The frame as drawn (DWMWA_EXTENDED_FRAME_BOUNDS, 9), else GetWindowRect; nullopt when
    // the window has gone.
    virtual std::optional<NativeRect> frame(WId window) = 0;
};

// The installed seams. Setting nullptr puts the real one back; a caller that installs one keeps
// it alive until it puts the previous one back.
ScreenGrabber& screenGrabber();
ScreenGrabber* setScreenGrabber(ScreenGrabber* grabber); // returns the one it replaced
Win32Desktop& win32Desktop();
Win32Desktop* setWin32Desktop(Win32Desktop* desktop);
WindowLister& windowLister();
WindowLister* setWindowLister(WindowLister* lister);

// The two names OpenWhisper decides by: sys.platform ("win32", "darwin", "linux") and
// QGuiApplication::platformName() ("windows", "cocoa", "xcb", "offscreen"). Tests pose as
// another desktop by overriding them; nullopt returns to the real answer. Each setter returns
// the override it replaced, so a nested pose can put the outer one back.
std::string systemPlatform();
std::optional<std::string> setSystemPlatform(std::optional<std::string> platform);
QString qtPlatformName();
std::optional<QString> setQtPlatformName(std::optional<QString> name);

// The real seams of this platform. The screen grabber is grabWindow(0) everywhere; off Windows
// the Win32 desktop and window lister throw std::runtime_error from every call, which only a
// test posing as Windows without installing the fakes can reach.
ScreenGrabber& realScreenGrabber();
Win32Desktop& realWin32Desktop();
WindowLister& realWindowLister();

} // namespace fv::ui_snap
