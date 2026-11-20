#include "ui_snap/desktop.h"

#include <utility>

#include <QGuiApplication>
#include <QPixmap>
#include <QScreen>

#include "base/platform_info.h"

namespace fv::ui_snap {
namespace {

class QtScreenGrabber final : public ScreenGrabber {
public:
    std::optional<QImage> grabScreen(QScreen& screen) override {
        const QPixmap pixmap = screen.grabWindow(0);
        if (pixmap.isNull()) {
            return std::nullopt;
        }
        return pixmap.toImage();
    }
};

struct Installed {
    ScreenGrabber* grabber = nullptr;
    Win32Desktop* win32 = nullptr;
    WindowLister* lister = nullptr;
    std::optional<std::string> system;
    std::optional<QString> qtPlatform;
};

Installed& installed() {
    static Installed seams;
    return seams;
}

template <class T> T* swap(T*& slot, T* value) {
    return std::exchange(slot, value);
}

} // namespace

ScreenGrabber& realScreenGrabber() {
    static QtScreenGrabber grabber;
    return grabber;
}

ScreenGrabber& screenGrabber() {
    ScreenGrabber* grabber = installed().grabber;
    return grabber != nullptr ? *grabber : realScreenGrabber();
}

ScreenGrabber* setScreenGrabber(ScreenGrabber* grabber) {
    return swap(installed().grabber, grabber);
}

Win32Desktop& win32Desktop() {
    Win32Desktop* desktop = installed().win32;
    return desktop != nullptr ? *desktop : realWin32Desktop();
}

Win32Desktop* setWin32Desktop(Win32Desktop* desktop) {
    return swap(installed().win32, desktop);
}

WindowLister& windowLister() {
    WindowLister* lister = installed().lister;
    return lister != nullptr ? *lister : realWindowLister();
}

WindowLister* setWindowLister(WindowLister* lister) {
    return swap(installed().lister, lister);
}

std::string systemPlatform() {
    const auto& system = installed().system;
    return system ? *system : std::string(python_platform());
}

std::optional<std::string> setSystemPlatform(std::optional<std::string> platform) {
    return std::exchange(installed().system, std::move(platform));
}

QString qtPlatformName() {
    const auto& name = installed().qtPlatform;
    return name ? *name : QGuiApplication::platformName();
}

std::optional<QString> setQtPlatformName(std::optional<QString> name) {
    return std::exchange(installed().qtPlatform, std::move(name));
}

} // namespace fv::ui_snap
