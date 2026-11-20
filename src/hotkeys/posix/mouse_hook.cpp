#include "hotkeys/mouse_hook.h"

#include <stdexcept>

#include "hotkeys/mouse_shortcuts.h"

namespace fv::hotkeys {

namespace {

class UnsupportedMouseHookSource final : public MouseHookSource {
public:
    void start(Claims, OnButton) override {
        throw std::runtime_error("Mouse button shortcuts are not supported on this platform.");
    }
    void stop() override {}
    bool running() const override { return false; }
    bool wait_until_installed(std::chrono::milliseconds) override { return false; }
};

} // namespace

std::unique_ptr<MouseHookSource> make_mouse_hook_source(MonotonicClock) {
    return std::make_unique<UnsupportedMouseHookSource>();
}

// Python reaches its Windows hook here too and fails to load user32; a walkthrough then reports
// that Auto Mode could not start.
std::unique_ptr<MouseHookSource> make_mouse_hook_source(ClickObserver, MonotonicClock) {
    return std::make_unique<UnsupportedMouseHookSource>();
}

// OpenWhisper_CPP's only hotkey backend is the Windows hook, so side buttons are bound nowhere
// else.
bool mouse_shortcuts_supported() {
    return false;
}

} // namespace fv::hotkeys
