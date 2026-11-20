// The real WindowLister: EnumWindows, WindowFromPoint and the DWM's frame and cloak attributes.
#include "ui_snap/desktop.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <dwmapi.h>

namespace fv::ui_snap {
namespace {

// DWMWINDOWATTRIBUTE values pointed.py passes by number.
constexpr DWORD kExtendedFrameBounds = 9;
constexpr DWORD kCloaked = 14;

HWND hwnd(WId window) {
    return reinterpret_cast<HWND>(window);
}

WId wid(HWND window) {
    return reinterpret_cast<WId>(window);
}

BOOL CALLBACK collect(HWND window, LPARAM into) {
    auto& windows = *reinterpret_cast<std::vector<ListedWindow>*>(into);
    ListedWindow listed;
    listed.handle = wid(window);
    listed.visible = IsWindowVisible(window) != FALSE;
    listed.iconic = IsIconic(window) != FALSE;
    DWORD cloaked = 0;
    if (DwmGetWindowAttribute(window, kCloaked, &cloaked, sizeof(cloaked)) == S_OK) {
        listed.cloaked = cloaked != 0;
    }
    windows.push_back(listed);
    return TRUE;
}

class RealWindowLister final : public WindowLister {
public:
    QPoint cursorPos() override {
        POINT point{};
        GetCursorPos(&point);
        return {point.x, point.y};
    }

    WId rootWindowAt(QPoint point) override {
        return wid(GetAncestor(WindowFromPoint(POINT{point.x(), point.y()}), GA_ROOT));
    }

    std::vector<ListedWindow> windows() override {
        std::vector<ListedWindow> windows;
        EnumWindows(collect, reinterpret_cast<LPARAM>(&windows));
        return windows;
    }

    std::optional<NativeRect> frame(WId window) override {
        RECT rect{};
        if (DwmGetWindowAttribute(hwnd(window), kExtendedFrameBounds, &rect, sizeof(rect)) !=
                S_OK &&
            GetWindowRect(hwnd(window), &rect) == FALSE) {
            return std::nullopt;
        }
        return NativeRect{rect.left, rect.top, rect.right, rect.bottom};
    }
};

} // namespace

WindowLister& realWindowLister() {
    static RealWindowLister lister;
    return lister;
}

} // namespace fv::ui_snap
