#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QScreen>
#include <QString>

#include "ui_snap/desktop.h"

// Fakes of the Snap desktop seams: captures are synthetic images, windows and the foreground
// are tables the test fills, and every call is recorded.
namespace fv::ui_snap::testing {

// A screen-sized RGB32 image of #336699 per call, or what `image` returns; `fail` throws.
class FakeScreenGrabber final : public ScreenGrabber {
public:
    std::optional<QImage> grabScreen(QScreen& screen) override {
        calls.push_back(&screen);
        if (fail) {
            throw std::runtime_error("capture failed");
        }
        if (image) {
            return image(screen);
        }
        return screenImage(screen);
    }

    static QImage screenImage(const QScreen& screen, double scale = 1.0) {
        const QSize size = screen.geometry().size() * scale;
        QImage frozen(size, QImage::Format_RGB32);
        frozen.fill(QColor(QStringLiteral("#336699")));
        return frozen;
    }

    std::function<std::optional<QImage>(QScreen&)> image;
    bool fail = false;
    std::vector<QScreen*> calls;
};

class FakeWin32Desktop final : public Win32Desktop {
public:
    struct AffinityCall {
        WId window = 0;
        std::uint32_t affinity = 0;
        friend bool operator==(const AffinityCall&, const AffinityCall&) = default;
    };

    bool setWindowDisplayAffinity(WId window, std::uint32_t affinity) override {
        affinityCalls.push_back({window, affinity});
        if (affinityThrows) {
            throw std::runtime_error("user32 is unavailable");
        }
        if (affinityResult) {
            affinities[window] = affinity;
        }
        return affinityResult;
    }

    std::optional<std::uint32_t> windowDisplayAffinity(WId window) override {
        const auto found = affinities.find(window);
        if (found == affinities.end()) {
            return 0;
        }
        return found->second;
    }

    WId foregroundWindow() override { return foreground; }

    std::uint32_t windowThreadProcessId(WId window) override {
        const auto found = threads.find(window);
        return found == threads.end() ? 0 : found->second;
    }

    std::uint32_t currentThreadId() override { return ourThread; }

    bool attachThreadInput(std::uint32_t from, std::uint32_t to, bool attach) override {
        calls.push_back((attach ? "attach " : "detach ") + std::to_string(from) + " " +
                        std::to_string(to));
        return attachResult;
    }

    bool setForegroundWindow(WId window) override {
        calls.push_back("foreground " + std::to_string(window));
        foreground = window;
        return true;
    }

    std::optional<NativeRect> monitorAt(QPoint point, bool nearest) override {
        monitorQueries.emplace_back(point, nearest);
        if (monitorThrows) {
            throw std::runtime_error("GetMonitorInfoW failed");
        }
        return monitor;
    }

    std::vector<AffinityCall> affinityCalls;
    bool affinityResult = true;
    bool affinityThrows = false;
    std::map<WId, std::uint32_t> affinities;

    WId foreground = 0;
    std::map<WId, std::uint32_t> threads;
    std::uint32_t ourThread = 1;
    bool attachResult = true;
    std::vector<std::string> calls; // "attach 7 1", "foreground 42", "detach 7 1"

    std::optional<NativeRect> monitor;
    bool monitorThrows = false;
    std::vector<std::pair<QPoint, bool>> monitorQueries;
};

class FakeWindowLister final : public WindowLister {
public:
    QPoint cursorPos() override { return cursor; }

    WId rootWindowAt(QPoint point) override {
        pointQueries.push_back(point);
        return underCursor;
    }

    std::vector<ListedWindow> windows() override {
        if (fail) {
            throw std::runtime_error("EnumWindows failed");
        }
        return listed;
    }

    std::optional<NativeRect> frame(WId window) override {
        const auto found = frames.find(window);
        if (found == frames.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    // A visible, uncloaked window of `rect` behind the ones added before it.
    void add(WId handle, NativeRect rect) {
        listed.push_back({handle, true, false, false});
        frames[handle] = rect;
    }

    QPoint cursor;
    WId underCursor = 0;
    std::vector<QPoint> pointQueries;
    std::vector<ListedWindow> listed; // front to back
    std::map<WId, NativeRect> frames;
    bool fail = false;
};

// Installs the three fakes and poses as a desktop (Windows by default) for its lifetime, then
// puts back what was there, an outer ScopedFakeDesktop's fakes and pose included.
class ScopedFakeDesktop {
public:
    explicit ScopedFakeDesktop(std::string system = "win32",
                               QString qtPlatform = QStringLiteral("windows"))
        : previousGrabber_(setScreenGrabber(&grabber)), previousWin32_(setWin32Desktop(&win32)),
          previousLister_(setWindowLister(&lister)),
          previousSystem_(setSystemPlatform(std::move(system))),
          previousQtPlatform_(setQtPlatformName(std::move(qtPlatform))) {}

    ~ScopedFakeDesktop() {
        setScreenGrabber(previousGrabber_);
        setWin32Desktop(previousWin32_);
        setWindowLister(previousLister_);
        setSystemPlatform(std::move(previousSystem_));
        setQtPlatformName(std::move(previousQtPlatform_));
    }

    ScopedFakeDesktop(const ScopedFakeDesktop&) = delete;
    ScopedFakeDesktop& operator=(const ScopedFakeDesktop&) = delete;

    FakeScreenGrabber grabber;
    FakeWin32Desktop win32;
    FakeWindowLister lister;

private:
    ScreenGrabber* previousGrabber_;
    Win32Desktop* previousWin32_;
    WindowLister* previousLister_;
    std::optional<std::string> previousSystem_;
    std::optional<QString> previousQtPlatform_;
};

} // namespace fv::ui_snap::testing
