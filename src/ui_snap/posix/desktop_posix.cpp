// Win32 seams off Windows: nothing reaches them unless a test poses as Windows without
// installing a fake, so every call says so.
#include "ui_snap/desktop.h"

#include <stdexcept>

namespace fv::ui_snap {
namespace {

[[noreturn]] void unsupported() {
    throw std::runtime_error("not supported on this platform");
}

class PosixWin32Desktop final : public Win32Desktop {
public:
    bool setWindowDisplayAffinity(WId, std::uint32_t) override { unsupported(); }
    std::optional<std::uint32_t> windowDisplayAffinity(WId) override { unsupported(); }
    WId foregroundWindow() override { unsupported(); }
    std::uint32_t windowThreadProcessId(WId) override { unsupported(); }
    std::uint32_t currentThreadId() override { unsupported(); }
    bool attachThreadInput(std::uint32_t, std::uint32_t, bool) override { unsupported(); }
    bool setForegroundWindow(WId) override { unsupported(); }
    std::optional<NativeRect> monitorAt(QPoint, bool) override { unsupported(); }
};

class PosixWindowLister final : public WindowLister {
public:
    QPoint cursorPos() override { unsupported(); }
    WId rootWindowAt(QPoint) override { unsupported(); }
    std::vector<ListedWindow> windows() override { unsupported(); }
    std::optional<NativeRect> frame(WId) override { unsupported(); }
};

} // namespace

Win32Desktop& realWin32Desktop() {
    static PosixWin32Desktop desktop;
    return desktop;
}

WindowLister& realWindowLister() {
    static PosixWindowLister lister;
    return lister;
}

} // namespace fv::ui_snap
