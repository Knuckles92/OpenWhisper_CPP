// The real Win32Desktop: user32's display affinity, foreground and monitor calls.
#include "ui_snap/desktop.h"

#include <system_error>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace fv::ui_snap {
namespace {

HWND hwnd(WId window) {
    return reinterpret_cast<HWND>(window);
}

WId wid(HWND window) {
    return reinterpret_cast<WId>(window);
}

class RealWin32Desktop final : public Win32Desktop {
public:
    bool setWindowDisplayAffinity(WId window, std::uint32_t affinity) override {
        return SetWindowDisplayAffinity(hwnd(window), affinity) != FALSE;
    }

    std::optional<std::uint32_t> windowDisplayAffinity(WId window) override {
        DWORD affinity = 0;
        if (GetWindowDisplayAffinity(hwnd(window), &affinity) == FALSE) {
            return std::nullopt;
        }
        return affinity;
    }

    WId foregroundWindow() override { return wid(GetForegroundWindow()); }

    std::uint32_t windowThreadProcessId(WId window) override {
        return GetWindowThreadProcessId(hwnd(window), nullptr);
    }

    std::uint32_t currentThreadId() override { return GetCurrentThreadId(); }

    bool attachThreadInput(std::uint32_t from, std::uint32_t to, bool attach) override {
        return AttachThreadInput(from, to, attach ? TRUE : FALSE) != FALSE;
    }

    bool setForegroundWindow(WId window) override {
        return SetForegroundWindow(hwnd(window)) != FALSE;
    }

    std::optional<NativeRect> monitorAt(QPoint point, bool nearest) override {
        const POINT native{point.x(), point.y()};
        HMONITOR monitor =
            MonitorFromPoint(native, nearest ? MONITOR_DEFAULTTONEAREST : MONITOR_DEFAULTTONULL);
        if (monitor == nullptr) {
            return std::nullopt;
        }
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info) == FALSE) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "GetMonitorInfoW");
        }
        const RECT& rect = info.rcMonitor;
        return NativeRect{rect.left, rect.top, rect.right, rect.bottom};
    }
};

} // namespace

Win32Desktop& realWin32Desktop() {
    static RealWin32Desktop desktop;
    return desktop;
}

} // namespace fv::ui_snap
